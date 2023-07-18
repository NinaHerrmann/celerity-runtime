#include "worker_job.h"

#include <spdlog/fmt/fmt.h>

#include "buffer_manager.h"
#include "closure_hydrator.h"
#include "device_queue.h"
#include "handler.h"
#include "reduction_manager.h"
#include "runtime.h"
#include "task_manager.h"
#include "workaround.h"

namespace celerity {
namespace detail {

	// --------------------------------------------------------------------------------------------------------------------
	// ----------------------------------------------------- GENERAL ------------------------------------------------------
	// --------------------------------------------------------------------------------------------------------------------

	bool worker_job::prepare() {
		CELERITY_LOG_SET_SCOPED_CTX(m_lctx);

		// if(!m_tracy_lane.is_initialized()) {
		// 	m_tracy_lane.initialize();
		// 	const auto desc = fmt::format("cid={}: {}", m_pkg.cid, get_description(m_pkg));
		// 	m_tracy_lane.begin_phase("preparation", desc, tracy::Color::ColorType::Pink);
		// 	CELERITY_DEBUG("Preparing job: {}", desc);
		// }

		// m_tracy_lane.activate();
		const auto result = prepare(m_pkg);
		// m_tracy_lane.deactivate();
		return result;
	}

	void worker_job::update() {
		CELERITY_LOG_SET_SCOPED_CTX(m_lctx);
		assert(m_running && !m_done);
		const auto before = std::chrono::steady_clock::now();
		m_done = execute(m_pkg);

		// TODO: We may want to make benchmarking optional with a macro
		const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before);
		m_bench_sum_execution_time += dt;
		m_bench_sample_count++;
		if(dt < m_bench_min) m_bench_min = dt;
		if(dt > m_bench_max) m_bench_max = dt;

		if(m_done) {
			const auto bench_avg = m_bench_sum_execution_time.count() / m_bench_sample_count;
			const auto execution_time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_start_time).count();
			CELERITY_DEBUG("Job finished after {}us. Polling avg={}, min={}, max={}, samples={}", execution_time, bench_avg, m_bench_min.count(),
			    m_bench_max.count(), m_bench_sample_count);
		}
	}

	void worker_job::start() {
		CELERITY_LOG_SET_SCOPED_CTX(m_lctx);
		assert(!m_running);
		m_running = true;

		CELERITY_DEBUG("Starting job: {}", get_description(m_pkg));
		m_start_time = std::chrono::steady_clock::now();
	}

	// --------------------------------------------------------------------------------------------------------------------
	// --------------------------------------------------- HORIZON --------------------------------------------------------
	// --------------------------------------------------------------------------------------------------------------------

	std::string horizon_job::get_description(const command_pkg& pkg) { return "horizon"; }

	bool horizon_job::execute(const command_pkg& pkg) {
		const auto data = std::get<horizon_data>(pkg.data);
		m_task_mngr.notify_horizon_reached(data.tid);
		return true;
	};

	// --------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------- EPOCH ---------------------------------------------------------
	// --------------------------------------------------------------------------------------------------------------------

	std::string epoch_job::get_description(const command_pkg& pkg) { return "epoch"; }

	bool epoch_job::execute(const command_pkg& pkg) {
		const auto data = std::get<epoch_data>(pkg.data);
		m_action = data.action;

		// This barrier currently enables profiling Celerity programs on a cluster by issuing a queue.slow_full_sync() and
		// then observing the execution times of barriers. TODO remove this once we have a better profiling workflow.
		if(m_action == epoch_action::barrier) { MPI_Barrier(MPI_COMM_WORLD); }

		m_task_mngr.notify_epoch_reached(data.tid);
		return true;
	};

	// --------------------------------------------------------------------------------------------------------------------
	// --------------------------------------------------- AWAIT PUSH -----------------------------------------------------
	// --------------------------------------------------------------------------------------------------------------------

	std::string await_push_job::get_description(const command_pkg& pkg) {
		const auto data = std::get<await_push_data>(pkg.data);
		return fmt::format("await push of buffer {} transfer {}", static_cast<size_t>(data.bid), static_cast<size_t>(data.trid));
	}

	bool await_push_job::execute(const command_pkg& pkg) {
		if(m_data_handle == nullptr) {
			const auto data = std::get<await_push_data>(pkg.data);
			m_data_handle = m_btm.await_push(data.trid, data.bid, data.region, data.rid);
		}
		return m_data_handle->complete;
	}


	// --------------------------------------------------------------------------------------------------------------------
	// ------------------------------------------------------- PUSH -------------------------------------------------------
	// --------------------------------------------------------------------------------------------------------------------

	std::string push_job::get_description(const command_pkg& pkg) {
		const auto data = std::get<push_data>(pkg.data);
		return fmt::format("push {} of buffer {} transfer {} to node {}", data.sr, static_cast<size_t>(data.bid), static_cast<size_t>(data.trid),
		    static_cast<size_t>(data.target));
	}

	inline constexpr size_t send_recv_unit_bytes = 64; // NOCOMMIT FIXME: Copy of value in buffer_transfer_manager.cc

	bool push_job::prepare(const command_pkg& pkg) {
		if(m_frame.get_pointer() == nullptr) {
			// ZoneScopedN("push_job::prepare");
			const auto data = std::get<push_data>(pkg.data);
			// Getting buffer data from the buffer manager may incur a host-side buffer reallocation.
			// If any other tasks are currently using this buffer for reading, we run into problems.
			// To avoid this, we use a very crude buffer locking mechanism for now.
			// FIXME: Get rid of this, replace with finer grained approach.
			if(m_buffer_mngr.is_locked(data.bid, 0 /* FIXME: Host memory id - should use host_queue::get_memory_id */)) { return false; }

			const auto element_size = m_buffer_mngr.get_buffer_info(data.bid).element_size;
			unique_frame_ptr<buffer_transfer_manager::data_frame> frame(
			    from_payload_count, data.sr.range.size() * element_size, /* packet_size_bytes */ send_recv_unit_bytes);
			frame->sr = data.sr;
			frame->bid = data.bid;
			frame->rid = data.rid;
			frame->trid = data.trid;
			m_frame_transfer_event = m_buffer_mngr.get_buffer_data(data.bid, data.sr, frame->data);
			m_frame = std::move(frame);
		}
		return m_frame_transfer_event.is_done();
	}

	bool push_job::execute(const command_pkg& pkg) {
		const auto data = std::get<push_data>(pkg.data);
		if(m_data_handle == nullptr) {
			assert(m_frame_transfer_event.is_done());
			CELERITY_TRACE("Submit buffer to BTM");
			m_data_handle = m_btm.push(data.target, std::move(m_frame));
		}
		return m_data_handle->complete;
	}

	// --------------------------------------------------------------------------------------------------------------------
	// ----------------------------------------------------- REDUCTION ----------------------------------------------------
	// --------------------------------------------------------------------------------------------------------------------

	bool reduction_job::execute(const command_pkg& pkg) {
		const auto& data = std::get<reduction_data>(pkg.data);
		m_rm.finish_reduction(data.rid);
		return true;
	}

	std::string reduction_job::get_description(const command_pkg& pkg) { return "reduction"; }

	// --------------------------------------------------------------------------------------------------------------------
	// --------------------------------------------------- HOST_EXECUTE ---------------------------------------------------
	// --------------------------------------------------------------------------------------------------------------------

	std::string host_execute_job::get_description(const command_pkg& pkg) {
		const auto data = std::get<execution_data>(pkg.data);
		return fmt::format("HOST_EXECUTE {}", data.sr);
	}

	bool host_execute_job::execute(const command_pkg& pkg) {
		if(!m_submitted) {
			const auto data = std::get<execution_data>(pkg.data);

			auto tsk = m_task_mngr.get_task(data.tid);
			assert(tsk->get_execution_target() == execution_target::host);
			assert(!data.initialize_reductions); // For now, we do not support reductions in host tasks

			if(!m_buffer_mngr.try_lock(pkg.cid, m_queue.get_memory_id(), tsk->get_buffer_access_map().get_accessed_buffers())) { return false; }

			CELERITY_TRACE("Scheduling host task in thread pool");

			const auto& access_map = tsk->get_buffer_access_map();
			std::vector<closure_hydrator::accessor_info> access_infos;
			access_infos.reserve(access_map.get_num_accesses());
			for(size_t i = 0; i < access_map.get_num_accesses(); ++i) {
				const auto [bid, mode] = access_map.get_nth_access(i);
				const auto sr = grid_box_to_subrange(access_map.get_requirements_for_nth_access(i, tsk->get_dimensions(), data.sr, tsk->get_global_size()));
				const auto info = m_buffer_mngr.access_host_buffer(bid, mode, sr);
				access_infos.push_back(closure_hydrator::accessor_info{info.ptr, info.backing_buffer_range, info.backing_buffer_offset, sr});
			}

			closure_hydrator::get_instance().arm(target::host_task, std::move(access_infos));
			m_future = tsk->launch(m_queue, data.sr);

			assert(m_future.valid());
			m_submitted = true;
			CELERITY_TRACE("Submitted host task to thread pool");
		}

		assert(m_future.valid());
		if(m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			m_buffer_mngr.unlock(pkg.cid);

			auto info = m_future.get();
			CELERITY_TRACE("Delta time submit -> start: {}us, start -> end: {}us",
			    std::chrono::duration_cast<std::chrono::microseconds>(info.start_time - info.submit_time).count(),
			    std::chrono::duration_cast<std::chrono::microseconds>(info.end_time - info.start_time).count());
			return true;
		}
		return false;
	}

	// --------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------- DEVICE_EXECUTE ------------------------------------------------
	// --------------------------------------------------------------------------------------------------------------------

	std::string device_execute_job::get_description(const command_pkg& pkg) {
		const auto data = std::get<execution_data>(pkg.data);
		return fmt::format("DEVICE_EXECUTE {} on device {}", data.sr, m_queue.get_id());
	}

	bool device_execute_job::prepare(const command_pkg& pkg) {
		// NOCOMMIT TODO: Rebasing this right now - not sure what the purpose is. Can prepare() be called even after it returns true once?
		if(m_async_transfers_done) return true;

		if(!m_async_transfers_submitted) {
			const auto data = std::get<execution_data>(pkg.data);
			auto tsk = m_task_mngr.get_task(data.tid);
			assert(tsk->get_execution_target() == execution_target::device);

			if(!m_buffer_mngr.try_lock(pkg.cid, m_queue.get_memory_id(), tsk->get_buffer_access_map().get_accessed_buffers())) { return false; }

			CELERITY_TRACE("Dispatch data transfers");

			const auto& access_map = tsk->get_buffer_access_map();
			const auto& reductions = tsk->get_reductions();
			m_accessor_infos.reserve(access_map.get_num_accesses());
			m_accessor_transfer_events.reserve(access_map.get_num_accesses());
			m_reduction_ptrs.reserve(reductions.size());
			// {
			// 	const auto msg = fmt::format("Preparing buffers for {} accesses", access_map.get_num_accesses());
			// 	TracyMessage(msg.c_str(), msg.size());
			// 	CELERITY_TRACE(msg);
			// }
			for(size_t i = 0; i < access_map.get_num_accesses(); ++i) {
				const auto [bid, mode] = access_map.get_nth_access(i);
				const auto sr = grid_box_to_subrange(access_map.get_requirements_for_nth_access(i, tsk->get_dimensions(), data.sr, tsk->get_global_size()));

				try {
					auto info = m_buffer_mngr.access_device_buffer(m_queue.get_memory_id(), bid, mode, sr);
#if CELERITY_ACCESSOR_BOUNDARY_CHECK
					auto* const oob_idx = sycl::malloc_shared<id<3>>(2, m_queue.get_sycl_queue());
					assert(oob_idx != nullptr);
					constexpr size_t size_t_max = std::numeric_limits<size_t>::max();
					const auto buffer_dims = m_buffer_mngr.get_buffer_info(bid).dimensions;
					oob_idx[0] = id<3>{size_t_max, buffer_dims > 1 ? size_t_max : 0, buffer_dims == 3 ? size_t_max : 0};
					oob_idx[1] = id<3>{1, 1, 1};
					m_oob_indices_per_accessor.push_back(oob_idx);
					m_accessor_infos.push_back(closure_hydrator::accessor_info{info.ptr, info.backing_buffer_range, info.backing_buffer_offset, sr, oob_idx});
#else
					m_accessor_infos.push_back(closure_hydrator::accessor_info{info.ptr, info.backing_buffer_range, info.backing_buffer_offset, sr});
#endif
					m_accessor_transfer_events.emplace_back(std::move(info.pending_transfers));
				} catch(allocation_error& e) {
					CELERITY_CRITICAL("Encountered allocation error while trying to prepare {}", get_description(pkg));
					std::terminate();
				}
			}

			for(size_t i = 0; i < reductions.size(); ++i) {
				const auto& rd = reductions[i];
				const auto mode = rd.init_from_buffer ? access_mode::read_write : access_mode::discard_write;
				// HACK: Shift accessed element by one for each GPU
				const auto did = m_queue.get_id();
				const auto info = m_buffer_mngr.access_device_buffer(m_queue.get_memory_id(), rd.bid, mode, subrange<3>{{did, 0, 0}, range<3>{1, 1, 1}});
				while(!info.pending_transfers.is_done()) {} // There is probably no point in trying to overlap this with anything
				m_reduction_ptrs.push_back(info.ptr);
			}

			// {
			// 	const auto msg = fmt::format("Preparing buffers for {} accesses", access_map.get_num_accesses());
			// 	TracyMessage(msg.c_str(), msg.size());
			// 	CELERITY_TRACE(msg);
			// }

			m_async_transfers_submitted = true;
		}

		if(!m_async_transfers_done
		    && std::all_of(m_accessor_transfer_events.cbegin(), m_accessor_transfer_events.cend(), [](auto& te) { return te.is_done(); })) {
			m_async_transfers_done = true;
			// const auto msg = fmt::format("{}: Async transfers done", pkg.cid);
			// TracyMessage(msg.c_str(), msg.size());
			return true;
		}

		return false;
	}

	bool device_execute_job::execute(const command_pkg& pkg) {
		if(!m_submitted) {
			const auto data = std::get<execution_data>(pkg.data);
			auto tsk = m_task_mngr.get_task(data.tid);
			closure_hydrator::get_instance().arm(target::device, std::move(m_accessor_infos));
			// HACK: Only one chunk may initialize the reduction (assuming it will be on device 0, which, at the time of hacking, is true)
			//       This assumes that reduction chunks are NOT oversubscribed
			//       (Alternatively we could ensure that all per-GPU elements other than the first are reset to 0 after each reduction)
			const bool initialize_reductions = data.initialize_reductions && (data.HACK_total_local_reductions == 1 || m_queue.get_id() == 0);
			m_event = tsk->launch(m_queue, data.sr, m_reduction_ptrs, initialize_reductions);
			// {
			// 	const auto msg = fmt::format("{}: Job submitted to SYCL (blocked on transfers until now!)", pkg.cid);
			// 	TracyMessage(msg.c_str(), msg.size());
			// }

			m_submitted = true;
			CELERITY_TRACE("Kernel submitted to SYCL");
		}

		const auto status = m_event.get_info<cl::sycl::info::event::command_execution_status>();
		if(status == cl::sycl::info::event_command_status::complete) {
			m_buffer_mngr.unlock(pkg.cid);
			const auto data = std::get<execution_data>(pkg.data);
			auto tsk = m_task_mngr.get_task(data.tid);

#if CELERITY_ACCESSOR_BOUNDARY_CHECK
			for(size_t i = 0; i < m_oob_indices_per_accessor.size(); ++i) {
				const id<3>& oob_min = m_oob_indices_per_accessor[i][0];
				const id<3>& oob_max = m_oob_indices_per_accessor[i][1];

				if(oob_max != id<3>{1, 1, 1}) {
					const auto& access_map = tsk->get_buffer_access_map();
					const auto acc_sr =
					    grid_box_to_subrange(access_map.get_requirements_for_nth_access(i, tsk->get_dimensions(), data.sr, tsk->get_global_size()));
					const auto oob_sr = subrange<3>(oob_min, range_cast<3>(oob_max - oob_min));
					CELERITY_ERROR("Out-of-bounds access in kernel '{}' detected: Accessor {} for buffer {} attempted to access indices between {} which are "
					               "outside of mapped subrange {}",
					    tsk->get_debug_name(), i, access_map.get_nth_access(i).first, oob_sr, acc_sr);
				}
				sycl::free(m_oob_indices_per_accessor[i], m_queue.get_sycl_queue());
			}
#endif

			++(*data.HACK_executed_reductions);
			for(const auto& reduction : tsk->get_reductions()) {
				const auto element_size = m_buffer_mngr.get_buffer_info(reduction.bid).element_size;

				if(data.HACK_total_local_reductions > 1) {
					if(*data.HACK_executed_reductions == data.HACK_total_local_reductions) {
						// This was the last local reduction, perform second level of intermediate reduction across GPUs
						auto per_gpu_results = make_uninitialized_payload<std::byte>(data.HACK_total_local_reductions * element_size);
						m_buffer_mngr.get_buffer_data(reduction.bid, {{}, {data.HACK_total_local_reductions, 1, 1}}, per_gpu_results.get_pointer()).wait();
						auto local_result = m_reduction_mngr.HACK_reduce_per_gpu_results(
						    reduction.rid, m_local_nid, std::move(per_gpu_results), data.HACK_total_local_reductions);
						// We need to also write the result back to the buffer (instead of just pushing it to the reduction manager),
						// in case we'll have to transfer this data to remote nodes.
						memcpy(m_buffer_mngr.access_host_buffer(reduction.bid, access_mode::discard_write, {{}, {1, 1, 1}}).ptr, local_result.get_pointer(),
						    element_size);
						m_reduction_mngr.push_overlapping_reduction_data(reduction.rid, m_local_nid, std::move(local_result));
					}
				} else {
					auto operand = make_uninitialized_payload<std::byte>(element_size);
					m_buffer_mngr.get_buffer_data(reduction.bid, {{}, {1, 1, 1}}, operand.get_pointer()).wait();
					m_reduction_mngr.push_overlapping_reduction_data(reduction.rid, m_local_nid, std::move(operand));
				}
			}

			if(m_queue.is_profiling_enabled()) {
				const auto submit = std::chrono::nanoseconds(m_event.get_profiling_info<cl::sycl::info::event_profiling::command_submit>());
				const auto start = std::chrono::nanoseconds(m_event.get_profiling_info<cl::sycl::info::event_profiling::command_start>());
				const auto end = std::chrono::nanoseconds(m_event.get_profiling_info<cl::sycl::info::event_profiling::command_end>());

				CELERITY_TRACE("Delta time submit -> start: {}us, start -> end: {}us",
				    std::chrono::duration_cast<std::chrono::microseconds>(start - submit).count(),
				    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
			}
			return true;
		}
		return false;
	}

	// --------------------------------------------------------------------------------------------------------------------
	// -------------------------------------------------------- FENCE -----------------------------------------------------
	// --------------------------------------------------------------------------------------------------------------------

	std::string fence_job::get_description(const command_pkg& pkg) { return fmt::format("FENCE"); }

	bool fence_job::execute(const command_pkg& pkg) {
		const auto data = std::get<fence_data>(pkg.data);
		const auto tsk = m_task_mngr.get_task(data.tid);
		const auto promise = tsk->get_fence_promise();
		assert(promise != nullptr);

		promise->fulfill();

		return true;
	}

} // namespace detail
} // namespace celerity
