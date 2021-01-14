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

#include <range/v3/view/drop.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/slice.hpp>

#include <libsolutil/Common.h>

#include <cstddef>
#include <map>
#include <optional>


using namespace std;
using namespace solidity;
using namespace solidity::evmasm;

void Inliner::optimise()
{
	std::map<u256, AssemblyItems> inlinableBlocks;

	std::optional<size_t> lastTag;
	for (auto&& [index, item]: m_items | ranges::views::enumerate)
	{
		if (lastTag && SemanticInformation::breaksCSEAnalysisBlock(item, true))
		{
			if (
				item.type() == Operation &&
				item.instruction() == Instruction::JUMP && // TODO: what about JUMPI?
				index - *lastTag < 5) // TODO: reasonable heuristics
			{
				auto subrange = m_items | ranges::views::slice(*lastTag + 1, index + 1);
				if (!subrange.empty())
					inlinableBlocks.emplace(
						std::piecewise_construct,
						std::forward_as_tuple(m_items[*lastTag].data()),
						std::forward_as_tuple(ranges::begin(subrange), ranges::end(subrange))
					);
			}
			lastTag.reset();
		}
		if (item.type() == Tag)
			lastTag = index;
	}

	if (inlinableBlocks.empty())
		return;

	AssemblyItems newItems;
	bool skipOne = false;
	for (auto&& [item, nextItem]: ranges::views::zip(m_items, ranges::views::drop(m_items, 1)))
	{
		if (skipOne)
		{
			skipOne = false;
			continue;
		}
		if (
			item.type() == PushTag &&
			nextItem.type() == Operation &&
			nextItem.instruction() == Instruction::JUMP
		)
			if (auto const *inlinableBlock = util::valueOrNullptr(inlinableBlocks, item.data()))
			{
				newItems += *inlinableBlock;
				skipOne = true;
				continue;
			}
		newItems.emplace_back(move(item));
	}
	if (!skipOne)
		newItems.emplace_back(move(m_items.back()));
	m_items = move(newItems);
}
