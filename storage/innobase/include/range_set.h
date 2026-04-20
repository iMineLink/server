/*****************************************************************************

Copyright (c) 2013, 2026, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/range_set.h
Set of non-overlapping, non-adjacent integer ranges.
*******************************************************/

#pragma once

#include "univ.i"
#include <set>

/** Structure to store first and last value of range */
struct range_t
{
  uint32_t first;
  uint32_t last;
};

/** Sort the range based on first value of the range */
struct range_compare
{
  bool operator() (const range_t lhs, const range_t rhs) const
  {
    return lhs.first < rhs.first;
  }
};

using range_set_t= std::set<range_t, range_compare>;

/** Set of non-overlapping, non-adjacent integer ranges.
After any insertion the set always contains the minimum number of ranges
needed to cover the union of all previously inserted values/ranges: any two
ranges that would touch or overlap are merged. */
class range_set
{
private:
  range_set_t ranges;

  range_set_t::iterator find(uint32_t value) const
  {
    auto r_offset= ranges.lower_bound({value, value});
    const auto r_end= ranges.end();
    /* lower_bound returns the first range whose first >= value.
    If that range starts strictly above value, or we are past the end,
    the containing range (if any) is the previous one. */
    if (r_offset == r_end || r_offset->first > value)
    {
      if (r_offset == ranges.begin())
        return r_end;
      r_offset= std::prev(r_offset);
    }
    if (r_offset->first <= value && r_offset->last >= value)
      return r_offset;
    return r_end;
  }
public:
  /** Split the range and add two more ranges
  @param[in] range	range to be split
  @param[in] value	Value to be removed from range */
  void split_range(range_set_t::iterator range, uint32_t value)
  {
    range_t split1{range->first, value - 1};
    range_t split2{value + 1, range->last};

    /* Remove the existing element */
    ranges.erase(range);

    /* Insert the two elements */
    ranges.emplace(split1);
    ranges.emplace(split2);
  }

  /** Remove the value with the given range
  @param[in,out] range  range to be changed
  @param[in]	 value	value to be removed */
  void remove_within_range(range_set_t::iterator range, uint32_t value)
  {
    range_t new_range{range->first, range->last};
    if (value == range->first)
    {
      if (range->first == range->last)
      {
        ranges.erase(range);
        return;
      }
      else
        new_range.first++;
    }
    else if (value == range->last)
      new_range.last--;
    else if (range->first < value && range->last > value)
      return split_range(range, value);

    ranges.erase(range);
    ranges.emplace(new_range);
  }

  /** Remove the value from the ranges.
  @param[in]	value	Value to be removed. */
  void remove_value(uint32_t value)
  {
    if (empty())
      return;
    range_t new_range {value, value};
    range_set_t::iterator range= ranges.lower_bound(new_range);
    if (range == ranges.end())
      return remove_within_range(std::prev(range), value);

    if (range->first > value && range != ranges.begin())
      /* Iterate the previous ranges to delete */
      return remove_within_range(std::prev(range), value);
    return remove_within_range(range, value);
  }

  /** Try to merge new_range into the existing range pointed to by 'range'.
  If they overlap or are adjacent, 'range' is erased and replaced by the
  union of the two, and an iterator to the resulting range is returned.
  Otherwise the set is left untouched and end() is returned.
  @param[in] range     existing range to potentially extend
  @param[in] new_range range to be absorbed
  @return iterator to the merged range, or end() if no merge is possible */
  range_set_t::iterator add_within_range(range_set_t::iterator range,
                                         range_t new_range)
  {
    /* Disjoint and non-adjacent. Subtraction is used instead of adding
    one to avoid overflow near UINT32_MAX. */
    if ((new_range.first > range->last && new_range.first - range->last > 1) ||
        (range->first > new_range.last && range->first - new_range.last > 1))
      return ranges.end();

    /* new_range already fully contained in range */
    if (range->first <= new_range.first && range->last >= new_range.last)
      return range;

    range_t merged{range->first < new_range.first ? range->first
                                                  : new_range.first,
                   range->last  > new_range.last  ? range->last
                                                  : new_range.last};
    ranges.erase(range);
    return ranges.emplace(merged).first;
  }

  /** Add the range in the ranges set, merging with any existing ranges that
  overlap or are adjacent to it.
  @param[in] new_range range to be added */
  void add_range(range_t new_range)
  {
    if (ranges.empty())
    {
      ranges.emplace(new_range);
      return;
    }

    /* Find the first existing range whose first >= new_range.first. The
    candidate to merge with is that range or the one preceding it.
    Comparisons below use short-circuit evaluation to avoid overflow
    near UINT32_MAX. */
    auto it= ranges.lower_bound(new_range);
    if (it != ranges.begin())
    {
      auto prev= std::prev(it);
      if (prev->last >= new_range.first || prev->last + 1 == new_range.first)
        it= prev;
    }

    auto merged= (it != ranges.end())
      ? add_within_range(it, new_range) : ranges.end();

    if (merged == ranges.end())
    {
      /* No overlap/adjacency with any existing range: insert as new. */
      ranges.emplace(new_range);
      return;
    }

    /* The merged range may now overlap or be adjacent with subsequent
    ranges; absorb them too. */
    while (true)
    {
      auto next= std::next(merged);
      if (next == ranges.end() ||
          (next->first > merged->last && next->first - merged->last > 1))
        break;
      range_t next_range= *next;
      ranges.erase(next);
      merged= add_within_range(merged, next_range);
      ut_ad(merged != ranges.end());
    }
  }

  /** Add the value in the ranges
  @param[in] value  value to be added */
  void add_value(uint32_t value)
  {
    range_t new_range{value, value};
    add_range(new_range);
  }

  bool remove_if_exists(uint32_t value)
  {
    auto r_offset= find(value);
    if (r_offset != ranges.end())
    {
      remove_within_range(r_offset, value);
      return true;
    }
    return false;
  }

  bool contains(uint32_t value) const
  {
    return find(value) != ranges.end();
  }

  ulint size() { return ranges.size(); }
  void clear() { ranges.clear(); }
  bool empty() const { return ranges.empty(); }
  typename range_set_t::iterator begin() { return ranges.begin(); }
  typename range_set_t::iterator end() { return ranges.end(); }
};
