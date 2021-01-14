/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * @file Inliner.h
 * Inlines small code snippets by replacing JUMP with a copy of the code jumped to.
 */
#pragma once

#include <vector>

namespace solidity::evmasm
{
class AssemblyItem;
using AssemblyItems = std::vector<AssemblyItem>;

class Inliner
{
public:
	explicit Inliner(AssemblyItems& _items): m_items(_items) {}
	virtual ~Inliner() = default;

	void optimise();

private:
	AssemblyItems& m_items;
};

}
