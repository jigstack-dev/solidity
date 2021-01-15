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

#include <optional>


using namespace std;
using namespace solidity;
using namespace solidity::evmasm;

bool Inliner::isInlineCandidate(u256 const& _tag, InlinableBlock const& _block) const
{
	assertThrow(_block.items.size() > 0, OptimizerException, "");

	// Never inline tags that reference themselves.
	for (AssemblyItem const& item: _block.items)
		if (item.type() == PushTag)
			if (_tag == item.data())
				return false;

	// Always try to inline if there is at most one call to the block.
	if (_block.pushTagCount == 1)
		return true;
	return static_cast<size_t>(_block.items.size()) <= m_inlineMaxOpcodes;
}

map<u256, Inliner::InlinableBlock> Inliner::determineInlinableBlocks(AssemblyItems const& _items) const
{
	std::map<u256, ranges::span<AssemblyItem const>> inlinableBlockItems;
	std::map<u256, uint64_t> numPushTags;
	std::optional<size_t> lastTag;
	for (auto&& [index, item]: _items | ranges::views::enumerate)
	{
		// The number of PushTag's approximates the number of calls to a block.
		if (item.type() == PushTag)
			numPushTags[item.data()]++;

		if (lastTag && SemanticInformation::breaksCSEAnalysisBlock(item, true))
		{
			if (item == Instruction::JUMP)
				inlinableBlockItems.emplace(
					piecewise_construct,
					forward_as_tuple(_items[*lastTag].data()),
					forward_as_tuple(_items | ranges::views::slice(*lastTag + 1, index + 1))
				);
			lastTag.reset();
		}
		if (item.type() == Tag)
			lastTag = index;
	}

	map<u256, InlinableBlock> result;
	for (auto&& [tag, items]: inlinableBlockItems)
		if (uint64_t const* numPushes = util::valueOrNullptr(numPushTags, tag))
		{
			InlinableBlock block{items, *numPushes};
			if (isInlineCandidate(tag, block))
				result.emplace(std::make_pair(tag, InlinableBlock{items, *numPushes}));
		}
	return result;
}

namespace
{
optional<AssemblyItem::JumpType> determineJumpType(AssemblyItem::JumpType _intoBlock, AssemblyItem::JumpType _outOfBlock)
{
	auto jumpTypeToInt = [](AssemblyItem::JumpType _jumpType) -> int {
		switch (_jumpType)
		{
			case AssemblyItem::JumpType::IntoFunction:
				return +1;
			case AssemblyItem::JumpType::OutOfFunction:
				return -1;
			case AssemblyItem::JumpType::Ordinary:
				return 0;
		}
		assertThrow(false, OptimizerException, "");
	};
	switch (jumpTypeToInt(_intoBlock) + jumpTypeToInt(_outOfBlock))
	{
		case 0:
			return AssemblyItem::JumpType::Ordinary;
		case 1:
			return AssemblyItem::JumpType::IntoFunction;
		case -1:
			return AssemblyItem::JumpType::OutOfFunction;
		default:
			return nullopt;
	}
}
}

void Inliner::optimise()
{
	std::map<u256, InlinableBlock> inlinableBlocks = determineInlinableBlocks(m_items);

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
					if (auto jumpType = determineJumpType(nextItem.getJumpType(), inlinableBlock->items.back().getJumpType()))
					{
						newItems += inlinableBlock->items;
						newItems.back().setJumpType(*jumpType);
						++it;
						continue;
					}
		}
		newItems.emplace_back(item);
	}

	m_items = move(newItems);
}
