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
 * @file Inliner.cpp
 * Inlines small code snippets by replacing JUMP with a copy of the code jumped to.
 */

#include <libevmasm/Inliner.h>

#include <libevmasm/AssemblyItem.h>
#include <libevmasm/SemanticInformation.h>

#include <libsolutil/CommonData.h>

#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/slice.hpp>
#include <range/v3/view/span.hpp>

#include <libsolutil/Common.h>

#include <cstddef>
#include <map>
#include <optional>


using namespace std;
using namespace solidity;
using namespace solidity::evmasm;

bool Inliner::isInlineCandidate(ranges::span<AssemblyItem> _items) const
{
	return _items.size() < m_inlineMaxOpcodes;
}

void Inliner::optimise()
{
	std::map<u256, ranges::span<AssemblyItem>> inlinableBlocks;

	std::optional<size_t> lastTag;
	for (auto&& [index, item]: m_items | ranges::views::enumerate)
	{
		if (lastTag && SemanticInformation::breaksCSEAnalysisBlock(item, true))
		{
			if (item == Instruction::JUMP)
			{
				ranges::span<AssemblyItem> items(m_items | ranges::views::slice(*lastTag + 1, index + 1));
				if (isInlineCandidate(items))
					inlinableBlocks.emplace(make_pair(m_items[*lastTag].data(), items));
			}
			lastTag.reset();
		}
		if (item.type() == Tag)
			lastTag = index;
	}

	if (inlinableBlocks.empty())
		return;

	AssemblyItems newItems;
	for (auto it = m_items.begin(); it != m_items.end(); ++it)
	{
		AssemblyItem const& item = *it;
		if (next(it) != m_items.end())
		{
			AssemblyItem const& nextItem = *next(it);
			if (item.type() == PushTag && nextItem == Instruction::JUMP)
				if (auto const *inlinableBlock = util::valueOrNullptr(inlinableBlocks, item.data()))
				{
					newItems += *inlinableBlock;
					++it;
					continue;
				}
		}
		newItems.emplace_back(item);
	}

	m_items = move(newItems);
}
