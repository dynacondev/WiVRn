/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "vk_allocator.h"
#include "vk/check.h" // Assuming CHECK_VK is defined for error checking

// Constructor
vk_allocator::vk_allocator(const VmaAllocatorCreateInfo & info)
{
	CHECK_VK(vmaCreateAllocator(&info, &handle));
	if (!handle)
	{
		throw std::runtime_error("Failed to create Vulkan memory allocator");
	}
}

// Destructor
vk_allocator::~vk_allocator()
{
	if (handle)
	{
		vmaDestroyAllocator(handle);
		handle = nullptr;
	}
}

// Move constructor
vk_allocator::vk_allocator(vk_allocator && other) noexcept
        :
        handle(other.handle)
{
	other.handle = nullptr;
}

// Move assignment operator
vk_allocator & vk_allocator::operator=(vk_allocator && other) noexcept
{
	if (this != &other)
	{
		if (handle)
		{
			vmaDestroyAllocator(handle);
		}
		handle = other.handle;
		other.handle = nullptr;
	}
	return *this;
}