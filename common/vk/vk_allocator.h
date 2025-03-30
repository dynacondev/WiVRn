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

#pragma once

#include "utils/singleton.h"
#include "vk_mem_alloc.h"

class vk_allocator : public singleton<vk_allocator>
{
	VmaAllocator handle = nullptr;

public:
	// Constructor
	explicit vk_allocator(const VmaAllocatorCreateInfo & info);

	// Destructor
	~vk_allocator();

	// Deleted copy constructor and assignment operator
	vk_allocator(const vk_allocator &) = delete;
	vk_allocator & operator=(const vk_allocator &) = delete;

	// Move constructor
	vk_allocator(vk_allocator && other) noexcept;

	// Move assignment operator
	vk_allocator & operator=(vk_allocator && other) noexcept;

	// Conversion operator to VmaAllocator
	operator VmaAllocator() const
	{
		return handle;
	}

	// Utility function to check if the allocator is valid
	bool is_valid() const
	{
		return handle != nullptr;
	}
};