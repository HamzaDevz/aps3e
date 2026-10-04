#pragma once

#include "../VulkanAPI.h"
#include "device.h"
#include "memory.h"
#include "unique_resource.h"
#include <variant>

namespace vk
{
	enum : u32
	{
		VK_BUFFER_CREATE_ALLOW_NULL_RPCS3           = 0x10000000,   // If we cannot allocate memory for the buffer, just return an empty but valid object with a null handle.
		VK_BUFFER_CREATE_IGNORE_VMEM_PRESSURE_RPCS3 = 0x20000000,   // If we cannot allocate memory for the buffer, do not run recovery routine to recover VRAM. Crash or return empty handle immediately instead.

		VK_BUFFER_CREATE_SPECIAL_FLAGS_RPCS3 = (VK_BUFFER_CREATE_ALLOW_NULL_RPCS3 | VK_BUFFER_CREATE_IGNORE_VMEM_PRESSURE_RPCS3)
	};

	struct buffer_view : public unique_resource
	{
		VkBufferView value;
		VkBufferViewCreateInfo info = {};
		VkBuffer full_buffer;

		buffer_view(VkDevice dev, VkBuffer buffer, VkFormat format, VkDeviceSize offset, VkDeviceSize size);
		~buffer_view();

		buffer_view(const buffer_view&) = delete;
		buffer_view(buffer_view&&)      = delete;

		bool in_range(u32 address, u32 size, u32& offset) const;

	private:
		VkDevice m_device;
	};

	struct buffer;

	// A raw VkBuffer bound to a sub-range of an existing allocation.
	// Used on drivers where texel buffer views are too small (e.g. Mali maxTexelBufferElements = 64K):
	// vertex attribute data is instead fetched through an SSBO.
	struct sub_buffer : public unique_resource
	{
		VkBuffer full_buffer;

		struct info_t
		{
			u32 offset;
			u32 size;
		} info;

		VkBuffer value;

		sub_buffer(const buffer& buf, VkFormat format, VkDeviceSize offset, VkDeviceSize size);
		~sub_buffer();

		sub_buffer(const sub_buffer&) = delete;
		sub_buffer(sub_buffer&&)      = delete;

		bool in_range(u32 address, u32 size, u32& offset) const;

	private:
		VkDevice m_device;
	};

	// Wrapper that can hold either a texel buffer_view or a sub_buffer (SSBO view)
	struct buffer_upload : public unique_resource
	{
		~buffer_upload();
		buffer_upload() = default;
		buffer_upload(const buffer_upload&) = delete;
		buffer_upload(buffer_upload&&) = delete;

		bool is(VkBuffer buffer) const;
		bool in_range(u32 address, u32 size, u32& offset) const;

		using buffer_info = std::variant<VkBufferView, VkBuffer>;
		buffer_info get_buffer() const;
		u64 get_resource_id() const;

		static std::unique_ptr<buffer_upload> create(const render_device& dev, const buffer& buf, VkFormat format, VkDeviceSize offset, VkDeviceSize size);

		bool is_buffer_view;

	private:
		static std::unique_ptr<buffer_upload> create_with_buffer_view(VkDevice dev, VkBuffer buffer, VkFormat format, VkDeviceSize offset, VkDeviceSize size);
		static std::unique_ptr<buffer_upload> create_with_sub_buffer(const buffer& buf, VkFormat format, VkDeviceSize offset, VkDeviceSize size);

	public:
		union
		{
			buffer_view* view;
			sub_buffer* sub;
		} ptr;
	};

	struct buffer : public unique_resource
	{
		VkBuffer value;
		VkBufferCreateInfo info = {};
		std::unique_ptr<vk::memory_block> memory;

		buffer(
			const vk::render_device& dev,
			u64 size,
			const memory_type_info& memory_type,
			u32 access_flags,
			VkBufferUsageFlags usage,
			VkBufferCreateFlags flags,
			vmm_allocation_pool allocation_pool);

		buffer(
			const vk::render_device& dev,
			VkBufferUsageFlags usage,
			void* host_pointer,
			u64 size);

		~buffer();

		void* map(u64 offset, u64 size);
		void unmap();
		u32 size() const;

		buffer(const buffer&) = delete;
		buffer(buffer&&) = delete;

	public:
		VkDevice m_device;
	};
}
