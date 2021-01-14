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

bool Inliner::isInlineCandidate(ranges::span<AssemblyItem const> _items) const
{
	return _items.size() < m_inlineMaxOpcodes;
}

map<u256, ranges::span<AssemblyItem const>> Inliner::determineInlinableBlocks(AssemblyItems const& _items) const
{
	std::map<u256, ranges::span<AssemblyItem const>> inlinableBlocks;
	std::optional<size_t> lastTag;
	for (auto&& [index, item]: _items | ranges::views::enumerate)
	{
		if (lastTag && SemanticInformation::breaksCSEAnalysisBlock(item, true))
		{
			if (item == Instruction::JUMP)
			{
				ranges::span<AssemblyItem const> items(_items | ranges::views::slice(*lastTag + 1, index + 1));
				if (isInlineCandidate(items))
					inlinableBlocks.emplace(make_pair(_items[*lastTag].data(), items));
			}
			lastTag.reset();
		}
		if (item.type() == Tag)
			lastTag = index;
	}
	return inlinableBlocks;
}

namespace
{
AssemblyItem::JumpType determineJumpType(AssemblyItem::JumpType _intoBlock, AssemblyItem::JumpType _outOfBlock)
{
	static map<pair<AssemblyItem::JumpType, AssemblyItem::JumpType>, AssemblyItem::JumpType> jumpTypeMapping{
		/*
		 * ...{A}...
		 * JUMP(tag_1) // intoBlock: ordinary
		 * tag_1:
		 * ...{B}...
		 * JUMP(tag_2) // outOfBlock: ordinary
		 * tag_2:
		 * ...
		 *
		 * to
		 *
		 * ...{A}...
		 * ...{B}...
		 * JUMP(tag_2) // result: ordinary
		 * tag_2:
		 * ...
		 */
		{{AssemblyItem::JumpType::Ordinary, AssemblyItem::JumpType::Ordinary}, AssemblyItem::JumpType::Ordinary},
		/*
		 * ...{A}...
		 * JUMP(tag_1) // intoBlock: ordinary
		 * tag_1:
		 * ...{B}...
		 * PUSHTAG(someFunction)
		 * JUMP(tag_2) // outOfBlock: into
		 * someFunction:
		 * ...
		 *
		 * to
		 *
		 * ...{A}...
		 * ...{B}...
		 * PUSHTAG(someFunction)
		 * JUMP(tag_2) // result: into
		 * someFunction:
		 * ...
		 */
		{{AssemblyItem::JumpType::Ordinary, AssemblyItem::JumpType::IntoFunction}, AssemblyItem::JumpType::IntoFunction},
		/*
		 * PUSHTAG(returnTag)
		 * ...{A}...
		 * JUMP(tag_1) // intoBlock: ordinary
		 * tag_1:
		 * ...{B}...
		 * JUMP(tag_2) // outOfBlock: out of
		 * returnTag:
		 * ...
		 *
		 * to
		 *
		 * PUSHTAG(returnTag)
		 * ...{A}...
		 * ...{B}...
		 * JUMP(tag_2) // result: out of
		 * returnTag:
		 *
		 */
		{{AssemblyItem::JumpType::Ordinary, AssemblyItem::JumpType::OutOfFunction}, AssemblyItem::JumpType::OutOfFunction},
		/*
		 * ...{A}...
		 * PUSHTAG(main_out)
		 * JUMP(tag_1) // intoBlock: into
		 * main_out:
		 * ...{B}...
		 *
		 * tag_1:
		 * ...{C}...
		 * JUMP(tag_2) // outOfBlock: ordinary
		 *
		 * tag_2:
		 * ...<potentially more jumps>...
		 * JUMP // out, eventually
		 *
		 * to
		 *
		 * ...{A}...
		 * PUSHTAG(main_out)
		 * ...{C}...
		 * JUMP(tag_2) // result: into
		 * out_1:
		 * ...{B}...
		 *
		 * tag_2:
		 * ...<potentially more jumps>...
		 * JUMP // out, eventually
		 */
		{{AssemblyItem::JumpType::IntoFunction, AssemblyItem::JumpType::Ordinary}, AssemblyItem::JumpType::IntoFunction},
		/*
		 * ...{A}...
		 * PUSHTAG(main_out)
		 * JUMP(tag_1) // intoBlock: into
		 * main_out:
		 * ...{B}...
		 *
		 * tag_1:
		 * ...{C}...
		 * PUSHTAG(tag_1_out)
		 * JUMP(tag_2) // outOfBlock: into
		 * tag_1_out:
		 * ...<potentially more jumps>...
		 * JUMP // out, eventually
		 *
		 * tag_2:
		 * ...<potentially more jumps>...
		 * JUMP // out, eventually
		 *
		 * to
		 *
		 * ...{A}...
		 * PUSHTAG(main_out)
		 * ...{C}...
		 * PUSHTAG(tag_1_out)
		 * JUMP(tag_2) // result: into (2x)
		 * main_out:
		 * ...{B}...
		 *
		 * tag_1_out:
		 * ...<potentially more jumps>...
		 * JUMP // out, eventually
		 *
		 * tag_2:
		 * ...<potentially more jumps>...
		 * JUMP // out, eventually
		 */
		{{AssemblyItem::JumpType::IntoFunction, AssemblyItem::JumpType::IntoFunction}, AssemblyItem::JumpType::IntoFunction},
		/*
		 * PUSHTAG(main_out)
		 * JUMP(f) // intoBlock: into
		 * main_out:
		 * ...
		 *
		 * f:
		 * ...{A}...
		 * JUMP // outOfBlock: out
		 *
		 * to
		 *
		 * PUSHTAG(main_out)
		 * ...{A}...
		 * JUMP // outOfBlock: ordinary
		 * main_out:
		 * ...
		 *
		 *
		 */
		{{AssemblyItem::JumpType::IntoFunction, AssemblyItem::JumpType::OutOfFunction}, AssemblyItem::JumpType::Ordinary},
		/*
		 * ...{A}...
		 * JUMP(return) // intoBlock: out of
		 * return:
		 * ...{B}...
		 * JUMP(tag_2) // outOfBlock: ordinary
		 *
		 * tag_2:
		 * ...
		 *
		 * to
		 *
		 * ...{A}...
		 * ...{B}...
		 * JUMP(tag_2) // result: out of
		 *
		 * tag_2:
		 * ...
		 *
		 *
		 */
		{{AssemblyItem::JumpType::OutOfFunction, AssemblyItem::JumpType::Ordinary}, AssemblyItem::JumpType::OutOfFunction},
		/*
		 * ...{A}...
		 * JUMP(return) // intoBlock: out of
		 * return:
		 * ...{B}...
		 * PUSH(tag_2_out)
		 * JUMP(tag_2) // outOfBlock: into
		 * tag_2_out:
		 * ...{C}...
		 *
		 * tag_2:
		 * ...<potentially more jumps>...
		 * JUMP // out, eventually
		 *
		 * to
		 *
		 * ...{A}...
		 * ...{B}...
		 * PUSH(tag_2_out)
		 * JUMP(tag_2) // result: ordinary
		 * tag2_out:
		 * ...{C}...
		 *
		 * tag_2:
		 * ...<potentially more jumps>...
		 * JUMP // out, eventually
		 */
		{{AssemblyItem::JumpType::OutOfFunction, AssemblyItem::JumpType::IntoFunction}, AssemblyItem::JumpType::Ordinary},
		/*
		 * ...{A}...
		 * JUMP(return) // intoBlock: out of
		 *
		 * return:
		 * ...{C}...
		 * JUMP(return2) // outOfBlock: out of
		 *
		 * return2:
		 * ...{D}...
		 *
		 * to
		 *
		 * ...{A}...
		 * ...{C}...
		 * JUMP(return2) // outOfBlock: out of (2x)
		 *
		 * return2:
		 * ...{D}...
		 *
		 */
		{{AssemblyItem::JumpType::OutOfFunction, AssemblyItem::JumpType::OutOfFunction}, AssemblyItem::JumpType::OutOfFunction},
	};
	return jumpTypeMapping.at(make_pair(_intoBlock, _outOfBlock));
}
}

void Inliner::optimise()
{
	std::map<u256, ranges::span<AssemblyItem const>> inlinableBlocks = determineInlinableBlocks(m_items);

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
					newItems.back().setJumpType(determineJumpType(nextItem.getJumpType(), inlinableBlock->back().getJumpType()));
					++it;
					continue;
				}
		}
		newItems.emplace_back(item);
	}

	m_items = move(newItems);
}
