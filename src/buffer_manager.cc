#include "buffer_manager.h"

#include "buffer_storage.h"
#include "log.h"
#include "runtime.h"

namespace celerity {
namespace detail {

	buffer_manager::buffer_manager(device_queue& queue, buffer_lifecycle_callback lifecycle_cb) : m_queue(queue), m_lifecycle_cb(std::move(lifecycle_cb)) {}

	void buffer_manager::unregister_buffer(buffer_id bid) noexcept {
		{
			std::unique_lock lock(m_mutex);
			assert(m_buffer_infos.find(bid) != m_buffer_infos.end());

			// Log the allocation size for host and device
			const auto& buf = m_buffers[bid];
			const size_t host_size = buf.host_buf.is_allocated() ? buf.host_buf.storage->get_size() : 0;
			const size_t device_size = buf.device_buf.is_allocated() ? buf.device_buf.storage->get_size() : 0;

			CELERITY_TRACE("Unregistering buffer {}. host size = {} B, device size = {} B", bid, host_size, device_size);
			m_buffers.erase(bid);
			m_buffer_infos.erase(bid);

#if defined(CELERITY_DETAIL_ENABLE_DEBUG)
			m_buffer_types.erase(bid);
#endif
		}
		m_lifecycle_cb(buffer_lifecycle_event::unregistered, bid);
	}

	void buffer_manager::get_buffer_data(buffer_id bid, const subrange<3>& sr, void* out_linearized) {
		std::unique_lock lock(m_mutex);
		assert(m_buffers.count(bid) == 1 && (m_buffers.at(bid).device_buf.is_allocated() || m_buffers.at(bid).host_buf.is_allocated()));
		auto data_locations = m_newest_data_location.at(bid).get_region_values(subrange_to_grid_box(sr));

		// Slow path: We need to obtain current data from both host and device.
		if(data_locations.size() > 1) {
			auto& existing_buf = m_buffers[bid].host_buf;
			assert(existing_buf.is_allocated());

			// Make sure newest data resides on the host.
			// But first, we need to check whether the current host buffer is able to hold the full data range.
			const auto info = is_resize_required(existing_buf, sr.range, sr.offset);
			backing_buffer replacement_buf;
			if(info.resize_required) {
				// TODO: Do we really want to allocate host memory for this..? We could also make the buffer storage "coherent" directly.
				replacement_buf = backing_buffer{m_buffer_infos.at(bid).construct_host(info.new_range), info.new_offset};
			}
			existing_buf = make_buffer_subrange_coherent(bid, access_mode::read, std::move(existing_buf), sr, std::move(replacement_buf));

			data_locations = {{subrange_to_grid_box(sr), data_location::host}};
		}

		// get_buffer_data will race with pending transfers for the same subrange. In case there are pending transfers and a host buffer does not exist yet,
		// these transfers cannot easily be flushed here as creating a host buffer requires a templated context that knows about DataT.
		assert(std::none_of(m_scheduled_transfers[bid].begin(), m_scheduled_transfers[bid].end(),
		    [&](const transfer& t) { return subrange_to_grid_box(sr).intersectsWith(subrange_to_grid_box(t.sr)); }));

		if(data_locations[0].second == data_location::host || data_locations[0].second == data_location::host_and_device) {
			return m_buffers.at(bid).host_buf.storage->get_data({m_buffers.at(bid).host_buf.get_local_offset(sr.offset), sr.range}, out_linearized);
		}

		return m_buffers.at(bid).device_buf.storage->get_data({m_buffers.at(bid).device_buf.get_local_offset(sr.offset), sr.range}, out_linearized);
	}

	void buffer_manager::set_buffer_data(buffer_id bid, const subrange<3>& sr, unique_payload_ptr in_linearized) {
		std::unique_lock lock(m_mutex);
		assert(m_buffer_infos.count(bid) == 1);
		m_scheduled_transfers[bid].push_back({std::move(in_linearized), sr});
	}

	buffer_manager::access_info buffer_manager::access_device_buffer(buffer_id bid, access_mode mode, const subrange<3>& sr) {
		std::unique_lock lock(m_mutex);
		assert((range_cast<3>(sr.offset + sr.range) <= m_buffer_infos.at(bid).range) == cl::sycl::range<3>(true, true, true));

		auto& existing_buf = m_buffers[bid].device_buf;
		backing_buffer replacement_buf;

		if(!existing_buf.is_allocated()) {
			replacement_buf = backing_buffer{m_buffer_infos.at(bid).construct_device(sr.range, m_queue.get_sycl_queue()), sr.offset};
		} else {
			// FIXME: For large buffers we might not be able to store two copies in device memory at once.
			// Instead, we'd first have to transfer everything to the host and free the old buffer before allocating the new one.
			// TODO: What we CAN do however already is to free the old buffer early iff we're requesting a discard_* access!
			// (AND that access request covers the entirety of the old buffer!)
			const auto info = is_resize_required(existing_buf, sr.range, sr.offset);
			if(info.resize_required) {
				replacement_buf = backing_buffer{m_buffer_infos.at(bid).construct_device(info.new_range, m_queue.get_sycl_queue()), info.new_offset};
			}
		}

		audit_buffer_access(bid, replacement_buf.is_allocated(), mode);

		if(m_test_mode && replacement_buf.is_allocated()) {
			auto* ptr = replacement_buf.storage->get_pointer();
			const auto bytes = replacement_buf.storage->get_size();
			m_queue.get_sycl_queue().submit([&](cl::sycl::handler& cgh) { cgh.memset(ptr, test_mode_pattern, bytes); }).wait();
		}

		existing_buf = make_buffer_subrange_coherent(bid, mode, std::move(existing_buf), {sr.offset, sr.range}, std::move(replacement_buf));

		return {existing_buf.storage->get_pointer(), existing_buf.storage->get_range(), existing_buf.offset};
	}

	buffer_manager::access_info buffer_manager::access_host_buffer(buffer_id bid, access_mode mode, const subrange<3>& sr) {
		std::unique_lock lock(m_mutex);
		assert((range_cast<3>(sr.offset + sr.range) <= m_buffer_infos.at(bid).range) == cl::sycl::range<3>(true, true, true));

		auto& existing_buf = m_buffers[bid].host_buf;
		backing_buffer replacement_buf;

		if(!existing_buf.is_allocated()) {
			replacement_buf = backing_buffer{m_buffer_infos.at(bid).construct_host(sr.range), sr.offset};
		} else {
			const auto info = is_resize_required(existing_buf, sr.range, sr.offset);
			if(info.resize_required) { replacement_buf = backing_buffer{m_buffer_infos.at(bid).construct_host(info.new_range), info.new_offset}; }
		}

		audit_buffer_access(bid, replacement_buf.is_allocated(), mode);

		if(m_test_mode && replacement_buf.is_allocated()) {
			auto* ptr = replacement_buf.storage->get_pointer();
			const auto size = replacement_buf.storage->get_size();
			std::memset(ptr, test_mode_pattern, size);
		}

		existing_buf = make_buffer_subrange_coherent(bid, mode, std::move(existing_buf), {sr.offset, sr.range}, std::move(replacement_buf));

		return {existing_buf.storage->get_pointer(), existing_buf.storage->get_range(), existing_buf.offset};
	}

	bool buffer_manager::try_lock(const buffer_lock_id id, const std::unordered_set<buffer_id>& buffers) {
		assert(m_buffer_locks_by_id.count(id) == 0);
		for(auto bid : buffers) {
			if(m_buffer_lock_infos[bid].is_locked) return false;
		}
		m_buffer_locks_by_id[id].reserve(buffers.size());
		for(auto bid : buffers) {
			m_buffer_lock_infos[bid] = {true, std::nullopt};
			m_buffer_locks_by_id[id].push_back(bid);
		}
		return true;
	}

	void buffer_manager::unlock(buffer_lock_id id) {
		assert(m_buffer_locks_by_id.count(id) != 0);
		for(auto bid : m_buffer_locks_by_id[id]) {
			m_buffer_lock_infos[bid] = {};
		}
		m_buffer_locks_by_id.erase(id);
	}

	bool buffer_manager::is_locked(buffer_id bid) const {
		if(m_buffer_lock_infos.count(bid) == 0) return false;
		return m_buffer_lock_infos.at(bid).is_locked;
	}

	// TODO: Something we could look into is to dispatch all memory copies concurrently and wait for them in the end.
	buffer_manager::backing_buffer buffer_manager::make_buffer_subrange_coherent(
	    buffer_id bid, cl::sycl::access::mode mode, backing_buffer existing_buffer, const subrange<3>& coherent_sr, backing_buffer replacement_buffer) {
		backing_buffer target_buffer, previous_buffer;
		if(replacement_buffer.is_allocated()) {
			assert(!existing_buffer.is_allocated() || replacement_buffer.storage->get_type() == existing_buffer.storage->get_type());
			target_buffer = std::move(replacement_buffer);
			previous_buffer = std::move(existing_buffer);
		} else {
			assert(existing_buffer.is_allocated());
			target_buffer = std::move(existing_buffer);
			previous_buffer = {};
		}

		if(coherent_sr.range.size() == 0) { return target_buffer; }

		const auto target_buffer_location = target_buffer.storage->get_type() == buffer_type::host_buffer ? data_location::host : data_location::device;

		const auto coherent_box = subrange_to_grid_box(coherent_sr);

		// If a previous buffer is provided, we may have to retain some or all of the existing data.
		const GridRegion<3> retain_region = ([&]() {
			GridRegion<3> result = coherent_box;
			if(previous_buffer.is_allocated()) {
				result = GridRegion<3>::merge(result, subrange_to_grid_box({previous_buffer.offset, previous_buffer.storage->get_range()}));
			}
			return result;
		})(); // IIFE

		// Sanity check: Retain region must be at least as large as coherence box (and fully overlap).
		assert(coherent_box.area() <= retain_region.area());
		assert(GridRegion<3>::difference(coherent_box, retain_region).empty());
		// Also check that the new target buffer could actually fit the entire retain region.
		assert((grid_box_to_subrange(retain_region.boundingBox()).offset >= target_buffer.offset) == cl::sycl::id<3>(true, true, true));
		assert((grid_box_to_subrange(retain_region.boundingBox()).offset + grid_box_to_subrange(retain_region.boundingBox()).range
		           <= target_buffer.offset + target_buffer.storage->get_range())
		       == cl::sycl::id<3>(true, true, true));

		// Check whether we have any scheduled transfers that overlap with the requested subrange, and if so, apply them.
		// For this, we are not interested in the retain region (but we need to remember what parts will NOT have to be retained afterwards).
		auto remaining_region_after_transfers = retain_region;
#if !defined(CELERITY_DETAIL_ENABLE_DEBUG)
		// There should only be queued transfers for this buffer iff this is a consumer mode.
		// To assert this we check for bogus transfers for other modes in debug builds.
		if(detail::access::mode_traits::is_consumer(mode))
#endif
		{
			GridRegion<3> updated_region;
			std::vector<transfer> remaining_transfers;
			auto& scheduled_buffer_transfers = m_scheduled_transfers[bid];
			remaining_transfers.reserve(scheduled_buffer_transfers.size() / 2);
			for(auto& t : scheduled_buffer_transfers) {
				auto t_region = subrange_to_grid_box(t.sr);

				// Check whether this transfer applies to the current request.
				auto t_minus_coherent_region = GridRegion<3>::difference(t_region, coherent_box);
				if(!t_minus_coherent_region.empty()) {
					// Check if transfer applies partially.
					// This might happen in certain situations, when two different commands partially overlap in their required buffer ranges.
					// We currently handle this by only copying the part of the transfer that intersects with the requested buffer range
					// into the buffer, leaving the transfer around for future requests.
					//
					// NOTE: We currently assume that one of the requests will consume the FULL transfer. Only then we discard it.
					// This assumption is valid right now, as the graph generator will not consolidate adjacent pushes for two (or more)
					// separate commands. This might however change in the future.
					if(t_minus_coherent_region != t_region) {
						assert(detail::access::mode_traits::is_consumer(mode));
						auto intersection = GridRegion<3>::intersect(t_region, coherent_box);
						remaining_region_after_transfers = GridRegion<3>::difference(remaining_region_after_transfers, intersection);
						const auto element_size = m_buffer_infos.at(bid).element_size;
						intersection.scanByBoxes([&](const GridBox<3>& box) {
							auto sr = grid_box_to_subrange(box);
							// TODO can this temp buffer be avoided?
							auto tmp = make_uninitialized_payload<std::byte>(sr.range.size() * element_size);
							linearize_subrange(t.linearized.get_pointer(), tmp.get_pointer(), element_size, t.sr.range, {sr.offset - t.sr.offset, sr.range});
							target_buffer.storage->set_data({target_buffer.get_local_offset(sr.offset), sr.range}, tmp.get_pointer());
							updated_region = GridRegion<3>::merge(updated_region, box);
						});
					}
					// Transfer only applies partially, or not at all - which means we have to keep it around.
					remaining_transfers.emplace_back(std::move(t));
					continue;
				}

				// Transfer applies fully.
				assert(detail::access::mode_traits::is_consumer(mode));
				remaining_region_after_transfers = GridRegion<3>::difference(remaining_region_after_transfers, t_region);
				target_buffer.storage->set_data({target_buffer.get_local_offset(t.sr.offset), t.sr.range}, t.linearized.get_pointer());
				updated_region = GridRegion<3>::merge(updated_region, t_region);
			}
			// The target buffer now has the newest data in this region.
			m_newest_data_location.at(bid).update_region(updated_region, target_buffer_location);
			scheduled_buffer_transfers = std::move(remaining_transfers);
		}

		if(!remaining_region_after_transfers.empty()) {
			const auto maybe_retain_box = [&](const GridBox<3>& box) {
				if(detail::access::mode_traits::is_consumer(mode)) {
					// If we are accessing the buffer using a consumer mode, we have to retain the full previous contents, otherwise...
					const auto box_sr = grid_box_to_subrange(box);
					target_buffer.storage->copy(
					    *previous_buffer.storage, previous_buffer.get_local_offset(box_sr.offset), target_buffer.get_local_offset(box_sr.offset), box_sr.range);
				} else {
					// ...check if there are parts of the previous buffer that we are not going to overwrite (and thus have to retain).
					// If so, copy only those parts.
					const auto remaining_region = GridRegion<3>::difference(box, coherent_box);
					remaining_region.scanByBoxes([&](const GridBox<3>& small_box) {
						const auto small_box_sr = grid_box_to_subrange(small_box);
						target_buffer.storage->copy(*previous_buffer.storage, previous_buffer.get_local_offset(small_box_sr.offset),
						    target_buffer.get_local_offset(small_box_sr.offset), small_box_sr.range);
					});
				}
			};

			GridRegion<3> replicated_region;
			auto& buffer_data_locations = m_newest_data_location.at(bid);
			const auto data_locations = buffer_data_locations.get_region_values(remaining_region_after_transfers);
			for(auto& dl : data_locations) {
				// Note that this assertion can fail in legitimate cases, e.g.
				// when users manually handle uninitialized reads in the first iteration of some loop.
				// assert(!previous_buffer.is_allocated() || dl.second != data_location::NOWHERE);

				if(target_buffer.storage->get_type() == buffer_type::device_buffer) {
					// Copy from device in case we are resizing an existing buffer
					if((dl.second == data_location::device || dl.second == data_location::host_and_device) && previous_buffer.is_allocated()) {
						maybe_retain_box(dl.first);
					}
					// Copy from host, unless we are using a pure producer mode
					else if(dl.second == data_location::host && detail::access::mode_traits::is_consumer(mode)) {
						assert(m_buffers[bid].host_buf.is_allocated());
						const auto box_sr = grid_box_to_subrange(dl.first);
						const auto& host_buf = m_buffers[bid].host_buf;
						target_buffer.storage->copy(
						    *host_buf.storage, host_buf.get_local_offset(box_sr.offset), target_buffer.get_local_offset(box_sr.offset), box_sr.range);
						replicated_region = GridRegion<3>::merge(replicated_region, dl.first);
					}
				} else if(target_buffer.storage->get_type() == buffer_type::host_buffer) {
					// Copy from device, unless we are using a pure producer mode
					if(dl.second == data_location::device && detail::access::mode_traits::is_consumer(mode)) {
						assert(m_buffers[bid].device_buf.is_allocated());
						const auto box_sr = grid_box_to_subrange(dl.first);
						const auto& device_buf = m_buffers[bid].device_buf;
						target_buffer.storage->copy(
						    *device_buf.storage, device_buf.get_local_offset(box_sr.offset), target_buffer.get_local_offset(box_sr.offset), box_sr.range);
						replicated_region = GridRegion<3>::merge(replicated_region, dl.first);
					}
					// Copy from host in case we are resizing an existing buffer
					else if((dl.second == data_location::host || dl.second == data_location::host_and_device) && previous_buffer.is_allocated()) {
						maybe_retain_box(dl.first);
					}
				}
			}

			// Finally, remember the fact that we replicated some regions to the new target location.
			buffer_data_locations.update_region(replicated_region, data_location::host_and_device);
		}

		if(detail::access::mode_traits::is_producer(mode)) { m_newest_data_location.at(bid).update_region(coherent_box, target_buffer_location); }

		return target_buffer;
	}

	void buffer_manager::audit_buffer_access(buffer_id bid, bool requires_allocation, cl::sycl::access::mode mode) {
		auto& lock_info = m_buffer_lock_infos[bid];

		// Buffer locking is currently opt-in, so if this buffer isn't locked, we won't check anything else.
		if(!lock_info.is_locked) return;

		if(lock_info.earlier_access_mode == std::nullopt) {
			// First access, all good.
			lock_info.earlier_access_mode = mode;
			return;
		}

		if(requires_allocation) {
			// Re-allocation of a buffer that is currently being accessed never works.
			throw std::runtime_error("You are requesting multiple accessors for the same buffer, with later ones requiring a larger part of the buffer, "
			                         "causing a backing buffer reallocation. "
			                         "This is currently unsupported. Try changing the order of your calls to buffer::get_access.");
		}

		if(!access::mode_traits::is_consumer(*lock_info.earlier_access_mode) && access::mode_traits::is_consumer(mode)) {
			// Accessing a buffer using a pure producer mode followed by a consumer mode breaks our coherence bookkeeping.
			throw std::runtime_error("You are requesting multiple accessors for the same buffer, using a discarding access mode first, followed by a "
			                         "non-discarding mode. This is currently unsupported. Try changing the order of your calls to buffer::get_access.");
		}

		// We only need to remember pure producer accesses.
		if(!access::mode_traits::is_consumer(mode)) { lock_info.earlier_access_mode = mode; }
	}

} // namespace detail
} // namespace celerity
