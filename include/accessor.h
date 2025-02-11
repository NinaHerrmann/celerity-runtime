#pragma once

#include <type_traits>

#include <CL/sycl.hpp>

#include "access_modes.h"
#include "buffer.h"
#include "buffer_storage.h"
#include "handler.h"
#include "sycl_wrappers.h"

namespace celerity {

template <int Dims>
class partition;

template <typename DataT, int Dims, access_mode Mode, target Target>
class accessor;

namespace detail {

	template <typename DataT, int Dims, access_mode Mode, target Target>
	class accessor_base {
	  public:
		static_assert(Dims > 0, "0-dimensional accessors NYI");
		static_assert(Dims <= 3, "accessors can only have 3 dimensions or less");
		using value_type = DataT;
		using reference = DataT&;
		using const_reference = const DataT&;
	};

	template <typename DataT, int Dims, access_mode Mode, typename... Args>
	accessor<DataT, Dims, Mode, target::device> make_device_accessor(Args&&...);

	template <typename DataT, int Dims, access_mode Mode, typename... Args>
	accessor<DataT, Dims, Mode, target::host_task> make_host_accessor(Args&&...);

	template <typename TagT>
	constexpr access_mode deduce_access_mode();

	template <typename TagT>
	constexpr access_mode deduce_access_mode_discard();

	template <typename TagT>
	constexpr target deduce_access_target();

	template <typename DataT, int Dims, cl::sycl::access::mode Mode, target Target, int Index>
	class accessor_subscript_proxy;

	struct accessor_testspy;

// Hack: DPC++ and ComputeCpp do not implement the SYCL 2020 sycl::local_accessor default constructor yet and always require a handler for construction.
// Since there is no SYCL handler in the pre-pass, we abuse inheritance and friend declarations to conjure a temporary sycl::handler that can be passed to the
// existing local_accessor constructor. This works because neither accessor implementation keep references to the handler internally or interacts with context
// surrounding the fake handler instance.
#if WORKAROUND_DPCPP
	// The DPC++ handler declares `template<...> friend class accessor<...>`, so we specialize sycl::accessor with a made-up type and have it inherit from
	// sycl::handler in order to be able to call the private constructor of sycl::handler.
	struct hack_accessor_specialization_type {};
	using hack_null_sycl_handler = cl::sycl::accessor<celerity::detail::hack_accessor_specialization_type, 0, cl::sycl::access::mode::read,
	    cl::sycl::access::target::host_buffer, cl::sycl::access::placeholder::true_t, void>;
#elif WORKAROUND_COMPUTECPP
	// ComputeCpp's sycl::handler has a protected constructor, so we expose it to the public through inheritance.
	class hack_null_sycl_handler : public sycl::handler {
	  public:
		hack_null_sycl_handler() : sycl::handler(nullptr) {}
	};
#endif

	template <int Dims>
	subrange<Dims> get_effective_sycl_accessor_subrange(id<Dims> device_buffer_offset, subrange<Dims> sr) {
#if WORKAROUND_COMPUTECPP || WORKAROUND_DPCPP
		// For ComputeCpp and DPC++, we allocate a unit-sized dummy backing buffer. ComputeCpp >= 2.8.0 does not support zero-sized placeholder accessors, so
		// we simply create a unit-sized (SYCL) accessor instead.
		if(sr.range.size() == 0) return {{}, unit_range};
#endif
		return {sr.offset - device_buffer_offset, sr.range};
	}

} // namespace detail
} // namespace celerity

#if WORKAROUND_DPCPP
// See declaration of celerity::detail::hack_accessor_specialization_type
template <>
class cl::sycl::accessor<celerity::detail::hack_accessor_specialization_type, 0, cl::sycl::access::mode::read, cl::sycl::access::target::host_buffer,
    cl::sycl::access::placeholder::true_t, void> : public handler {
  public:
	accessor() : handler{nullptr, false} {}
};
#endif

namespace celerity {

/**
 * Maps slices of the accessor backing buffer present on a host to the virtual global range of the Celerity buffer.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
class [[deprecated("host_memory_layout will be removed in favor of buffer_allocation_window in a future version of Celerity")]] host_memory_layout {
  public:
	/**
	 * Layout map for a single dimension describing the offset and strides of its hyperplanes.
	 *
	 * - A zero-dimensional layout corresponds to an individual data item and is not explicitly modelled in the dimension vector.
	 * - A one-dimensional layout is an interval of one-dimensional space and is fully described by global and local offsets and a count of data items (aka
	 * 0-dimensional hyperplanes).
	 * - A two-dimensional layout is modelled as an interval of rows, which manifests as an offset (a multiple of the row width) and a stride (the row width
	 * itself). Each row (aka 1-dimensional hyperplane) is modelled by the same one-dimensional layout.
	 * - and so on for arbitrary dimensioned layouts.
	 */
	class [[deprecated("host_memory_layout will be removed in favor of buffer_allocation_window in a future version of Celerity")]] dimension {
	  public:
		dimension() noexcept = default;

		dimension(size_t global_size, size_t global_offset, size_t local_size, size_t local_offset, size_t extent)
		    : global_size(global_size), global_offset(global_offset), local_size(local_size), local_offset(local_offset), extent(extent) {
			assert(global_offset >= local_offset);
			assert(global_size >= local_size);
		}

		size_t get_global_size() const { return global_size; }

		size_t get_local_size() const { return local_size; }

		size_t get_global_offset() const { return global_offset; }

		size_t get_local_offset() const { return local_offset; }

		size_t get_extent() const { return extent; }

	  private:
		size_t global_size{};
		size_t global_offset{};
		size_t local_size{};
		size_t local_offset{};
		size_t extent{};
	};

	class [[deprecated("host_memory_layout will be removed in favor of buffer_allocation_window in a future version of Celerity")]] dimension_vector {
	  public:
		dimension_vector(size_t size) : this_size(size) {}

		dimension& operator[](size_t idx) { return values[idx]; }
		const dimension& operator[](size_t idx) const { return values[idx]; }

		size_t size() const { return this_size; }

	  private:
		/**
		 * Since contiguous dimensions can be merged when generating the memory layout, host_memory_layout is not generic over a fixed dimension count
		 */
		constexpr static size_t max_dimensionality = 4;
		std::array<dimension, max_dimensionality> values;
		size_t this_size;
	};

	explicit host_memory_layout(const dimension_vector& dimensions) : dimensions(dimensions) {}

	/** The layout maps per dimension, in descending dimensionality */
	const dimension_vector& get_dimensions() const { return dimensions; }

  private:
	dimension_vector dimensions;
};
#pragma GCC diagnostic pop

/**
 * In addition to the usual per-item access through the subscript operator, accessors in distributed and collective host tasks can access the underlying memory
 * of the node-local copy of a buffer directly through `accessor::get_allocation_window()`. Celerity does not replicate buffers fully on all nodes unless
 * necessary, instead keeping an allocation of a subset that is resized as needed.
 *
 * buffer_allocation_window denotes how indices in the subrange assigned to one node (the _window_) map to the underlying buffer storage (the
 * _allocation_). The structure threrefore describes three subranges: The buffer, the allocation, and the window, with the latter being fully contained in the
 * former. Popular third-party APIs, such as HDF5 hyperslabs, can accept parameters from such an explicit description in one or the other form.
 */
template <typename T, int Dims>
class buffer_allocation_window {
  public:
	T* get_allocation() const { return allocation; }

	range<Dims> get_buffer_range() const { return buffer_range; }

	range<Dims> get_allocation_range() const { return allocation_range; }

	range<Dims> get_window_range() const { return window_range; }

	id<Dims> get_allocation_offset_in_buffer() const { return allocation_offset_in_buffer; }

	id<Dims> get_window_offset_in_buffer() const { return window_offset_in_buffer; }

	id<Dims> get_window_offset_in_allocation() const { return window_offset_in_buffer - allocation_offset_in_buffer; }

  private:
	T* allocation;
	range<Dims> buffer_range;
	range<Dims> allocation_range;
	range<Dims> window_range;
	id<Dims> allocation_offset_in_buffer;
	id<Dims> window_offset_in_buffer;

  public:
	buffer_allocation_window(T* allocation, const range<Dims>& buffer_range, const range<Dims>& allocation_range, const range<Dims>& window_range,
	    const id<Dims>& allocation_offset_in_buffer, const id<Dims>& window_offset_in_buffer)
	    : allocation(allocation), buffer_range(buffer_range), allocation_range(allocation_range), window_range(window_range),
	      allocation_offset_in_buffer(allocation_offset_in_buffer), window_offset_in_buffer(window_offset_in_buffer) {}

	template <typename, int, access_mode, target>
	friend class accessor;
};

/**
 * Celerity wrapper around SYCL accessors.
 *
 * @note The Celerity accessor currently does not support get_size, get_count, get_range, get_offset and get_pointer,
 * as their semantics in a distributed context are unclear.
 */
template <typename DataT, int Dims, access_mode Mode>
class accessor<DataT, Dims, Mode, target::device> : public detail::accessor_base<DataT, Dims, Mode, target::device> {
	friend struct detail::accessor_testspy;

  public:
	template <target Target = target::device, typename Functor>
	accessor(const buffer<DataT, Dims>& buff, handler& cgh, Functor rmfn) : accessor(construct<Target>(buff, cgh, rmfn)) {}

	template <typename Functor, typename TagT>
	accessor(const buffer<DataT, Dims>& buff, handler& cgh, Functor rmfn, TagT tag) : accessor(buff, cgh, rmfn) {}

	template <typename Functor, typename TagT>
	accessor(const buffer<DataT, Dims>& buff, handler& cgh, Functor rmfn, TagT tag, property::no_init no_init) : accessor(buff, cgh, rmfn) {}

	template <typename Functor, typename TagT>
	accessor(const buffer<DataT, Dims>& buff, handler& cgh, Functor rmfn, TagT tag, property_list prop_list) {
		// in this static assert it would be more relevant to use property_list type, but if a defined type is used then it is always false and
		// always fails to compile. Hence we use a templated type so that it only produces a compile error when the ctr is called.
		static_assert(detail::constexpr_false<TagT>,
		    "Currently it is not accepted to pass a property list to an accessor constructor. Please use the property celerity::no_init "
		    "as a last argument in the constructor");
	}

#if !defined(__SYCL_DEVICE_ONLY__)
	accessor(const accessor& other) : sycl_accessor(other.sycl_accessor) { init_from(other); }

	accessor& operator=(const accessor& other) {
		if(this != &other) {
			sycl_accessor = other.sycl_accessor;
			init_from(other);
		}
		return *this;
	}
#endif

	template <access_mode M = Mode, int D = Dims>
	std::enable_if_t<detail::access::mode_traits::is_producer(M) && M != access_mode::atomic && (D > 0), DataT&> operator[](id<Dims> index) const {
		return detail::ranged_sycl_access(sycl_accessor, buffer_range, index - index_offset);
	}

	template <access_mode M = Mode, int D = Dims>
	std::enable_if_t<detail::access::mode_traits::is_pure_consumer(M) && (D > 0), DataT> operator[](id<Dims> index) const {
		return detail::ranged_sycl_access(sycl_accessor, buffer_range, index - index_offset);
	}

#pragma GCC diagnostic push
	// Ignore deprecation warnings emitted by SYCL implementations (e.g. hipSYCL)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	template <access_mode M = Mode, int D = Dims>
	[[deprecated("Atomic accessors are deprecated as of SYCL 2020")]] std::enable_if_t<M == access_mode::atomic && (D > 0), cl::sycl::atomic<DataT>> operator[](
	    id<Dims> index) const {
		return detail::ranged_sycl_access(sycl_accessor, buffer_range, index - index_offset);
	}
#pragma GCC diagnostic pop

	template <int D = Dims>
	std::enable_if_t<(D > 1), detail::accessor_subscript_proxy<DataT, D, Mode, target::device, 1>> operator[](const size_t d0) const {
		return {*this, d0};
	}

	friend bool operator==(const accessor& lhs, const accessor& rhs) {
		return lhs.sycl_accessor == rhs.sycl_accessor && lhs.buffer_range == rhs.buffer_range && lhs.index_offset == rhs.index_offset;
	}

	friend bool operator!=(const accessor& lhs, const accessor& rhs) { return !(lhs == rhs); }

  private:
#if WORKAROUND_DPCPP || WORKAROUND(COMPUTECPP, 2, 7)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations" // target::gobal_buffer is now target::device, but only for very recent versions of DPC++
	using sycl_accessor_t = cl::sycl::accessor<DataT, Dims, Mode, cl::sycl::access::target::global_buffer, cl::sycl::access::placeholder::true_t>;
#pragma GCC diagnostic pop
#elif WORKAROUND_COMPUTECPP
	using sycl_accessor_t = cl::sycl::accessor<DataT, Dims, Mode, cl::sycl::access::target::device, cl::sycl::access::placeholder::true_t>;
#else
	using sycl_accessor_t = cl::sycl::accessor<DataT, Dims, Mode, cl::sycl::target::device>;
#endif

	template <typename T, int D, access_mode M, typename... Args>
	friend accessor<T, D, M, target::device> detail::make_device_accessor(Args&&...);

	// see init_from
	cl::sycl::handler* const* eventual_sycl_cgh = nullptr;
	sycl_accessor_t sycl_accessor;
	sycl::id<Dims> index_offset;
	sycl::range<Dims> buffer_range = detail::zero_range; // TODO remove this togehter with ComputeCpp < 2.8.0 support

	accessor(
	    cl::sycl::handler* const* eventual_sycl_cgh, cl::sycl::buffer<DataT, Dims>& buffer, const subrange<Dims>& effective_subrange, id<Dims> index_offset)
	    : eventual_sycl_cgh(eventual_sycl_cgh),
	      // We pass a range and offset here to avoid interference from SYCL, but the offset must be relative to the *backing buffer*.
	      sycl_accessor(sycl_accessor_t(buffer, effective_subrange.range, effective_subrange.offset)), index_offset(index_offset),
	      buffer_range(buffer.get_range()) {
		// SYCL 1.2.1 dictates that all kernel parameters must have standard layout.
		// However, since we are wrapping a SYCL accessor, this assertion fails for some implementations,
		// as it is currently unclear whether SYCL accessors must also have standard layout.
		// See https://github.com/KhronosGroup/SYCL-Docs/issues/94
		// static_assert(std::is_standard_layout<accessor>::value, "accessor must have standard layout");
	}

	// For DPC++, we must call the sycl_accessor_t constructor with arguments that need to be computed beforehand, so we pass the results from construct()
	// throught to the accessor(constructor_args) ctor.
	using constructor_args = std::tuple<cl::sycl::handler* const*, sycl_accessor_t, id<Dims>, range<Dims>>;

	explicit accessor(constructor_args&& args)
	    : eventual_sycl_cgh(std::get<0>(args)), sycl_accessor(std::get<1>(args)), index_offset(std::get<2>(args)), buffer_range(std::get<3>(args)) {
		// SYCL 1.2.1 dictates that all kernel parameters must have standard layout.
		// However, since we are wrapping a SYCL accessor, this assertion fails for some implementations,
		// as it is currently unclear whether SYCL accessors must also have standard layout.
		// See https://github.com/KhronosGroup/SYCL-Docs/issues/94
		// static_assert(std::is_standard_layout<accessor>::value, "accessor must have standard layout");
	}

	template <target Target, typename Functor>
	static constructor_args construct(const buffer<DataT, Dims>& buff, handler& cgh, Functor rmfn) {
		if(detail::is_prepass_handler(cgh)) {
			auto& prepass_cgh = dynamic_cast<detail::prepass_handler&>(cgh);
			prepass_cgh.add_requirement(detail::get_buffer_id(buff), std::make_unique<detail::range_mapper<Dims, Functor>>(rmfn, Mode, buff.get_range()));
#if WORKAROUND_DPCPP
			// DPC++ does not support SYCL 2020 default-constructible accessors and requires a buffer. As of 2021-08-18, DPC++ placeholder accessors do not
			// keep a reference to the buffer on the host, so having the buffer go out of scope right away will not cause any problems.
			cl::sycl::buffer<DataT, Dims> faux_buf{detail::range_cast<Dims>(range{1, 1, 1})};
			sycl_accessor_t sycl_accessor{faux_buf};
#else
			sycl_accessor_t sycl_accessor{};
#endif
			return {static_cast<cl::sycl::handler* const*>(nullptr), sycl_accessor, id<Dims>{}, detail::zero_range};
		} else {
			if(detail::get_handler_execution_target(cgh) != detail::execution_target::DEVICE) {
				throw std::runtime_error(
				    "Calling accessor constructor with device target is only allowed in parallel_for tasks."
				    "If you want to access this buffer from within a host task, please specialize the call using one of the *_host_task tags");
			}
			auto& live_cgh = dynamic_cast<detail::live_pass_device_handler&>(cgh);
			auto eventual_sycl_cgh = live_cgh.get_eventual_sycl_cgh();

			// It's difficult to figure out which stored range mapper corresponds to this constructor call, which is why we just call the raw mapper manually.
			const auto mapped_sr = live_cgh.apply_range_mapper<Dims>(rmfn, buff.get_range());
			const auto access_info = detail::runtime::get_instance().get_buffer_manager().get_device_buffer<DataT, Dims>(
			    detail::get_buffer_id(buff), Mode, detail::range_cast<3>(mapped_sr.range), detail::id_cast<3>(mapped_sr.offset));
			const auto effective_sr = detail::get_effective_sycl_accessor_subrange(access_info.offset, mapped_sr);
			const auto sycl_accessor = sycl_accessor_t(access_info.buffer, effective_sr.range, effective_sr.offset);

			return {eventual_sycl_cgh, sycl_accessor, mapped_sr.offset, access_info.buffer.get_range()};
		}
	}

	void init_from(const accessor& other) {
		eventual_sycl_cgh = other.eventual_sycl_cgh;
		index_offset = other.index_offset;
		buffer_range = other.buffer_range;

		// The call to sycl::handler::require must happen inside the SYCL CGF, but since get_access within the Celerity CGF is executed before
		// the submission to SYCL, it needs to be deferred. We capture a reference to a SYCL handler pointer owned by the live pass handler that is
		// initialized once the SYCL CGF is entered. We then abuse the copy constructor that is called implicitly when the lambda is copied for the SYCL
		// kernel submission to call require().
#if !defined(__SYCL_DEVICE_ONLY__) && !defined(SYCL_DEVICE_ONLY)
		if(eventual_sycl_cgh != nullptr && *eventual_sycl_cgh != nullptr) {
			(*eventual_sycl_cgh)->require(sycl_accessor);
			eventual_sycl_cgh = nullptr; // only `require` once
		}
#endif
	}
};

// Celerity Accessor Deduction Guides
template <typename T, int D, typename Functor, typename TagT>
accessor(const buffer<T, D>& buff, handler& cgh, Functor rmfn, TagT tag)
    -> accessor<T, D, detail::deduce_access_mode<TagT>(), detail::deduce_access_target<std::remove_const_t<TagT>>()>;

template <typename T, int D, typename Functor, typename TagT>
accessor(const buffer<T, D>& buff, handler& cgh, Functor rmfn, TagT tag, property::no_init no_init)
    -> accessor<T, D, detail::deduce_access_mode_discard<TagT>(), detail::deduce_access_target<std::remove_const_t<TagT>>()>;

template <typename T, int D, typename Functor, typename TagT>
accessor(const buffer<T, D>& buff, handler& cgh, Functor rmfn, TagT tag, property_list prop_list)
    -> accessor<T, D, detail::deduce_access_mode_discard<TagT>(), detail::deduce_access_target<std::remove_const_t<TagT>>()>;

//

template <typename DataT, int Dims, access_mode Mode>
class accessor<DataT, Dims, Mode, target::host_task> : public detail::accessor_base<DataT, Dims, Mode, target::host_task> {
  public:
	template <target Target = target::host_task, typename Functor>
	accessor(const buffer<DataT, Dims>& buff, handler& cgh, Functor rmfn) {
		static_assert(!std::is_same_v<Functor, range<Dims>>, "The accessor constructor overload for master-access tasks (now called 'host tasks') has "
		                                                     "been removed with Celerity 0.2.0. Please provide a range mapper instead.");

		if(detail::is_prepass_handler(cgh)) {
			auto& prepass_cgh = dynamic_cast<detail::prepass_handler&>(cgh);
			prepass_cgh.add_requirement(detail::get_buffer_id(buff), std::make_unique<detail::range_mapper<Dims, Functor>>(rmfn, Mode, buff.get_range()));
		} else {
			if constexpr(Target == target::host_task) {
				if(detail::get_handler_execution_target(cgh) != detail::execution_target::HOST) {
					throw std::runtime_error(
					    "Calling accessor constructor with host_buffer target is only allowed in host tasks."
					    "If you want to access this buffer from within a parallel_for task, please specialize the call using one of the non host tags");
				}
				auto& live_cgh = dynamic_cast<detail::live_pass_host_handler&>(cgh);
				// It's difficult to figure out which stored range mapper corresponds to this constructor call, which is why we just call the raw mapper
				// manually.
				const auto sr = live_cgh.apply_range_mapper<Dims>(rmfn, buff.get_range());
				auto access_info = detail::runtime::get_instance().get_buffer_manager().get_host_buffer<DataT, Dims>(
				    detail::get_buffer_id(buff), Mode, detail::range_cast<3>(sr.range), detail::id_cast<3>(sr.offset));

				mapped_subrange = sr;
				optional_buffer = &access_info.buffer;
				index_offset = access_info.offset;
				virtual_buffer_range = buff.get_range();
			}
		}
	}

	template <typename Functor, typename TagT>
	accessor(const buffer<DataT, Dims>& buff, handler& cgh, Functor rmfn, TagT tag) : accessor(buff, cgh, rmfn) {}

	/**
	 * TODO: As of ComputeCpp 2.5.0 they do not support no_init prop, hence this constructor is needed along with discard deduction guide.
	 *    but once they do this should be replace for a constructor that takes a prop list as an argument.
	 */
	template <typename Functor, typename TagT>
	accessor(const buffer<DataT, Dims>& buff, handler& cgh, Functor rmfn, TagT tag, property::no_init no_init) : accessor(buff, cgh, rmfn) {}

	template <typename Functor, typename TagT>
	accessor(const buffer<DataT, Dims>& buff, handler& cgh, Functor rmfn, TagT tag, property_list prop_list) {
		static_assert(detail::constexpr_false<TagT>,
		    "Currently it is not accepted to pass a property list to an accessor constructor. Please use the property celerity::no_init "
		    "as a last argument in the constructor");
	}

	template <access_mode M = Mode, int D = Dims>
	std::enable_if_t<detail::access::mode_traits::is_producer(M) && (D > 0), DataT&> operator[](id<Dims> index) const {
		return *(get_buffer().get_pointer() + get_linear_offset(index));
	}

	template <access_mode M = Mode, int D = Dims>
	std::enable_if_t<detail::access::mode_traits::is_pure_consumer(M) && (D > 0), DataT> operator[](id<Dims> index) const {
		return *(get_buffer().get_pointer() + get_linear_offset(index));
	}

	template <int D = Dims>
	std::enable_if_t<(D > 1), detail::accessor_subscript_proxy<DataT, D, Mode, target::host_task, 1>> operator[](const size_t d0) const {
		return {*this, d0};
	}

	/**
	 * @brief Returns a pointer to the underlying buffer.
	 *
	 * Since the physical (or "backing") buffer underlying a Celerity buffer might have a different size,
	 * it is not always safe to return a pointer, as the stride might not be what is expected.
	 *
	 * However, it is always possible to get a pointer if the full buffer is being accessed.
	 *
	 * This API will be deprecated and subsequently removed in a future version.
	 *
	 * @throws Throws a std::logic_error if the buffer cannot be accessed with the expected stride.
	 */
	DataT* get_pointer() const {
		bool illegal_access = false;
		if(index_offset != detail::id_cast<Dims>(id<3>{0, 0, 0})) { illegal_access = true; }
		// We can be a bit more lenient for 1D buffers, in that the backing buffer doesn't have to have the full size.
		// (Dereferencing the pointer outside of the requested range is UB anyways).
		if(Dims > 1 && get_buffer().get_range() != virtual_buffer_range) { illegal_access = true; }
		if(illegal_access) { throw std::logic_error("Buffer cannot be accessed with expected stride"); }
		return get_buffer().get_pointer();
	}

	friend bool operator==(const accessor& lhs, const accessor& rhs) {
		return (lhs.optional_buffer == rhs.optional_buffer || (lhs.optional_buffer && rhs.optional_buffer && *lhs.optional_buffer == *rhs.optional_buffer))
		       && lhs.index_offset == rhs.index_offset;
	}

	friend bool operator!=(const accessor& lhs, const accessor& rhs) { return !(lhs == rhs); }

	/**
	 * Returns a pointer to the buffer allocation local to this node along with a description how this potentially smaller allocation maps to the underlying
	 * virtual buffer and the partition available in the current host task.
	 *
	 * Accessing the returned allocation outside the window will lead to undefined results.
	 */
	template <int KernelDims>
	buffer_allocation_window<DataT, Dims> get_allocation_window(const partition<KernelDims>& part) const {
		// We already know the range mapper output for "chunk" from the constructor. The parameter is a purely semantic dependency which ensures that
		// this function is not called outside a host task.
		(void)part;

		return {
		    get_buffer().get_pointer(),
		    virtual_buffer_range,
		    get_buffer().get_range(),
		    mapped_subrange.range,
		    index_offset,
		    mapped_subrange.offset,
		};
	}

	/**
	 * Returns a pointer to the host-local backing buffer along with a mapping to the global virtual buffer.
	 *
	 * Each host keeps only part of the global (virtual) buffer locally. The layout information can be used, for example, to perform distributed I/O on the
	 * partial buffer present at each host.
	 */
	// TODO remove this together with host_memory_layout after a grace period
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	template <int KernelDims>
	[[deprecated("get_host_memory will be removed in a future version of Celerity. Use get_allocation_window instead")]] std::pair<DataT*, host_memory_layout>
	get_host_memory(const partition<KernelDims>& part) const {
		// We already know the range mapper output for "chunk" from the constructor. The parameter is a purely semantic dependency which ensures that
		// this function is not called outside a host task.
		(void)part;

		host_memory_layout::dimension_vector dimensions(Dims);
		for(int d = 0; d < Dims; ++d) {
			dimensions[d] = {/* global_size */ virtual_buffer_range[d],
			    /* global_offset */ mapped_subrange.offset[d],
			    /* local_size */ get_buffer().get_range()[d],
			    /* local_offset */ mapped_subrange.offset[d] - index_offset[d],
			    /* extent */ mapped_subrange.range[d]};
		}

		return {get_buffer().get_pointer(), host_memory_layout{dimensions}};
	}
#pragma GCC diagnostic pop

  private:
	template <typename T, int D, access_mode M, typename... Args>
	friend accessor<T, D, M, target::host_task> detail::make_host_accessor(Args&&...);

	// Subange of the accessor, as set by the range mapper or requested by the user (master node host tasks only).
	// This does not necessarily correspond to the backing buffer's range.
	subrange<Dims> mapped_subrange;

	mutable detail::host_buffer<DataT, Dims>* optional_buffer = nullptr;

	// Offset of the backing buffer relative to the virtual buffer.
	id<Dims> index_offset;

	// The range of the Celerity buffer as created by the user.
	// We only need this to check whether it is safe to call get_pointer() or not.
	range<Dims> virtual_buffer_range = detail::range_cast<Dims>(range<3>(0, 0, 0));

	/**
	 * Constructor for pre-pass.
	 */
	accessor() = default;

	/**
	 * Constructor for live-pass.
	 */
	accessor(subrange<Dims> mapped_subrange, detail::host_buffer<DataT, Dims>& buffer, id<Dims> backing_buffer_offset, range<Dims> virtual_buffer_range)
	    : mapped_subrange(mapped_subrange), optional_buffer(&buffer), index_offset(backing_buffer_offset), virtual_buffer_range(virtual_buffer_range) {}

	detail::host_buffer<DataT, Dims>& get_buffer() const {
		assert(optional_buffer != nullptr);
		return *optional_buffer;
	}

	size_t get_linear_offset(id<Dims> index) const { return detail::get_linear_index(get_buffer().get_range(), index - index_offset); }
};


template <typename DataT, int Dims = 1>
class local_accessor {
  private:
#if WORKAROUND_DPCPP || WORKAROUND(COMPUTECPP, 2, 6)
	using sycl_accessor = cl::sycl::accessor<DataT, Dims, cl::sycl::access::mode::read_write, cl::sycl::access::target::local>;
#else
	using sycl_accessor = cl::sycl::local_accessor<DataT, Dims>;
#endif

  public:
	using value_type = DataT;
	using reference = DataT&;
	using const_reference = const DataT&;
	using size_type = size_t;

	local_accessor() : sycl_acc{make_placeholder_sycl_accessor()}, allocation_size(detail::zero_range) {}

#if !defined(__SYCL_DEVICE_ONLY__) && !defined(SYCL_DEVICE_ONLY)
	local_accessor(const range<Dims>& allocation_size, handler& cgh) : sycl_acc{make_placeholder_sycl_accessor()}, allocation_size(allocation_size) {
		if(!detail::is_prepass_handler(cgh)) {
			auto& device_handler = dynamic_cast<detail::live_pass_device_handler&>(cgh);
			eventual_sycl_cgh = device_handler.get_eventual_sycl_cgh();
		}
	}

	local_accessor(const local_accessor& other)
	    : sycl_acc(other.sycl_cgh() ? sycl_accessor{other.allocation_size, *other.sycl_cgh()} : other.sycl_acc), allocation_size(other.allocation_size),
	      eventual_sycl_cgh(other.sycl_cgh() ? nullptr : other.eventual_sycl_cgh) {}
#else
	local_accessor(const range<Dims>& allocation_size, handler& cgh);
	local_accessor(const local_accessor&) = default;
#endif

	local_accessor& operator=(const local_accessor&) = default;

	size_type byte_size() const noexcept { return allocation_size.size() * sizeof(value_type); }

	size_type size() const noexcept { return allocation_size.size(); }

	size_type max_size() const noexcept { return sycl_acc.max_size(); }

	bool empty() const noexcept { return sycl_acc.empty(); }

	range<Dims> get_range() const { return allocation_size; }

	std::add_pointer_t<value_type> get_pointer() const noexcept { return sycl_acc.get_pointer(); }

	// Workaround: ComputeCpp's legacy clang-8 has trouble deducing the return type of operator[] with decltype(auto), so we derive it manually.
	// TODO replace trailing return type with decltype(auto) once we require the new ComputeCpp (experimental) compiler.
	template <typename Index>
	inline auto operator[](const Index& index) const -> decltype(std::declval<const sycl_accessor&>()[index]) {
		return sycl_acc[index];
	}

  private:
	sycl_accessor sycl_acc;
	range<Dims> allocation_size;
	cl::sycl::handler* const* eventual_sycl_cgh = nullptr;

	static sycl_accessor make_placeholder_sycl_accessor() {
#if WORKAROUND_DPCPP || WORKAROUND_COMPUTECPP
		detail::hack_null_sycl_handler null_cgh;
		return sycl_accessor{detail::zero_range, null_cgh};
#else
		return sycl_accessor{};
#endif
	}

	cl::sycl::handler* sycl_cgh() const { return eventual_sycl_cgh != nullptr ? *eventual_sycl_cgh : nullptr; }
};


namespace detail {
	template <typename DataT, int Dims, access_mode Mode, typename... Args>
	accessor<DataT, Dims, Mode, target::device> make_device_accessor(Args&&... args) {
		return {std::forward<Args>(args)...};
	}

	template <typename DataT, int Dims, access_mode Mode, typename... Args>
	accessor<DataT, Dims, Mode, target::host_task> make_host_accessor(Args&&... args) {
		return {std::forward<Args>(args)...};
	}

	template <typename TagT>
	constexpr access_mode deduce_access_mode() {
		if constexpr(std::is_same_v<const TagT, decltype(celerity::read_only)> || //
		             std::is_same_v<const TagT, decltype(celerity::read_only_host_task)>) {
			return access_mode::read;
		} else if constexpr(std::is_same_v<const TagT, decltype(celerity::read_write)> || //
		                    std::is_same_v<const TagT, decltype(celerity::read_write_host_task)>) {
			return access_mode::read_write;
		} else if constexpr(std::is_same_v<const TagT, decltype(celerity::write_only)> || //
		                    std::is_same_v<const TagT, decltype(celerity::write_only_host_task)>) {
			return access_mode::write;
		} else {
			static_assert(constexpr_false<TagT>, "Invalid access tag, expecting one of celerity::{read_only,read_write,write_only}[_host_task]");
		}
	}

	template <typename TagT>
	constexpr access_mode deduce_access_mode_discard() {
		if constexpr(std::is_same_v<const TagT, decltype(celerity::read_only)> || //
		             std::is_same_v<const TagT, decltype(celerity::read_only_host_task)>) {
			static_assert(constexpr_false<TagT>, "Invalid access mode + no_init");
		} else if constexpr(std::is_same_v<const TagT, decltype(celerity::read_write)> || //
		                    std::is_same_v<const TagT, decltype(celerity::read_write_host_task)>) {
			return access_mode::discard_read_write;
		} else if constexpr(std::is_same_v<const TagT, decltype(celerity::write_only)> || //
		                    std::is_same_v<const TagT, decltype(celerity::write_only_host_task)>) {
			return access_mode::discard_write;
		} else {
			static_assert(constexpr_false<TagT>, "Invalid access tag, expecting one of celerity::{read_only,read_write,write_only}[_host_task]");
		}
	}

	template <typename TagT>
	constexpr target deduce_access_target() {
		if constexpr(std::is_same_v<const TagT, decltype(celerity::read_only)> ||  //
		             std::is_same_v<const TagT, decltype(celerity::read_write)> || //
		             std::is_same_v<const TagT, decltype(celerity::write_only)>) {
			return target::device;
		} else if constexpr(std::is_same_v<const TagT, decltype(celerity::read_only_host_task)> ||  //
		                    std::is_same_v<const TagT, decltype(celerity::read_write_host_task)> || //
		                    std::is_same_v<const TagT, decltype(celerity::write_only_host_task)>) {
			return target::host_task;
		} else {
			static_assert(constexpr_false<TagT>, "Invalid access tag, expecting one of celerity::{read_only,read_write,write_only}[_host_task]");
		}
	}


	template <typename DataT, cl::sycl::access::mode Mode, target Target>
	class accessor_subscript_proxy<DataT, 3, Mode, Target, 2> {
		using AccessorT = celerity::accessor<DataT, 3, Mode, Target>;

	  public:
		accessor_subscript_proxy(const AccessorT& acc, const size_t d0, const size_t d1) : acc(acc), d0(d0), d1(d1) {}

		decltype(std::declval<AccessorT>()[{0, 0, 0}]) operator[](const size_t d2) const { return acc[{d0, d1, d2}]; }

	  private:
		const AccessorT& acc;
		size_t d0;
		size_t d1;
	};

	template <typename DataT, int Dims, cl::sycl::access::mode Mode, target Target>
	class accessor_subscript_proxy<DataT, Dims, Mode, Target, 1> {
		template <int D>
		using AccessorT = celerity::accessor<DataT, D, Mode, Target>;

	  public:
		accessor_subscript_proxy(const AccessorT<Dims>& acc, const size_t d0) : acc(acc), d0(d0) {}

		// Note that we currently have to use SFINAE over constexpr-if + decltype(auto), as ComputeCpp 2.6.0 has
		// problems inferring the correct type in some cases (e.g. when DataT == sycl::id<>).
		template <int D = Dims>
		std::enable_if_t<D == 2, decltype(std::declval<AccessorT<2>>()[{0, 0}])> operator[](const size_t d1) const {
			return acc[{d0, d1}];
		}

		template <int D = Dims>
		std::enable_if_t<D == 3, accessor_subscript_proxy<DataT, 3, Mode, Target, 2>> operator[](const size_t d1) const {
			return {acc, d0, d1};
		}

	  private:
		const AccessorT<Dims>& acc;
		size_t d0;
	};

} // namespace detail

} // namespace celerity
