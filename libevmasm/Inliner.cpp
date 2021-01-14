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
		 * JUMP(tag) // intoBlock: ordinary
		 * tag:
		 * ...{B}...
		 * f() // outOfBlock: into
		 *
		 * to
		 *
		 * ...{A}...
		 * ...{B}...
		 * f() // result: into
		 */
		{{AssemblyItem::JumpType::Ordinary, AssemblyItem::JumpType::IntoFunction}, AssemblyItem::JumpType::IntoFunction},
		/*
		 * function f() {
		 *   ...{A}...
		 *   JUMP(tag) // intoBlock: ordinary
		 *   tag:
		 *   ...{B}...
		 *   leave // outOfBlock: out of
		 * }
		 *
		 * to
		 *
		 * function f() {
		 *   ...{A}...
		 *   ...{B}...
		 *   leave // result: out of
		 * }
		 *
		 */
		{{AssemblyItem::JumpType::Ordinary, AssemblyItem::JumpType::OutOfFunction}, AssemblyItem::JumpType::OutOfFunction},
		/*
		 * f() // intoBlock: into
		 *
		 * function f() {
		 *   ...{A}...
		 *   JUMP(tag) // outOfBlock: ordinary
		 *   tag:
		 *   ...{B}...
		 * }
		 *
		 * to
		 *
		 * ...{A}...
		 * f() // result: into
		 *
		 * function f() {
		 *   ...{B}...
		 * }
		 */
		{{AssemblyItem::JumpType::IntoFunction, AssemblyItem::JumpType::Ordinary}, AssemblyItem::JumpType::IntoFunction},
		/*
		 * TODO: this one is weird...
		 */
		{{AssemblyItem::JumpType::IntoFunction, AssemblyItem::JumpType::IntoFunction}, AssemblyItem::JumpType::IntoFunction},
		/*
		 * f() // intoBlock: into
		 *
		 * function f() {
		 * ...{A}...
		 * leave // outOfBlock: out of
		 * }
		 *
		 * to
		 *
		 * ...{A}... (with a ``JUMP(tag) tag:`` after the block as result)
		 *
		 *
		 */
		{{AssemblyItem::JumpType::IntoFunction, AssemblyItem::JumpType::OutOfFunction}, AssemblyItem::JumpType::Ordinary},
		/*
		 * function f() {
		 *   ...{A}...
		 *   leave // intoBlock: out of
		 * }
		 *
		 * f()
		 * JUMP(tag) // ordinry: ordinary
		 * tag:
		 * ...{B}...

		 *
		 * to
		 *
		 * function f() {
		 *    ...{A}...
		 *    ...{B}...
		 *    leave // result: out of
		 * }
		 *
		 * f()
		 *
		 *
		 */
		{{AssemblyItem::JumpType::OutOfFunction, AssemblyItem::JumpType::Ordinary}, AssemblyItem::JumpType::OutOfFunction},
		/*
		 * function f() {
		 *    ...{A}...
		 *    leave // intoBlock: out of
		 * }
		 * function g() {
		 * 	  ...{B}...
		 * }
		 *
		 * f()
		 * g() // outOfBlock: into
		 *
		 * to
		 *
		 * function f_and_g() {
		 *    ...{A}...
		 *    JUMP(tag) // result: ordinary
		 *    tag:
		 *    ...{B}...
		 * }
		 *
		 *
		 *
		 */
		{{AssemblyItem::JumpType::OutOfFunction, AssemblyItem::JumpType::IntoFunction}, AssemblyItem::JumpType::Ordinary},
		/*
		 * TODO: this one is also weird...
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
