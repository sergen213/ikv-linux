#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <span>

namespace ikv::detail {
template<class Compare>
// FUN_00463150, with FUN_004630f0's exact median ties and FUN_004630a0's swaps.
// Compare is pure: -1/0/+1 for original records named by the two indices.
void source_sort_order(std::span<std::uint32_t> order, Compare compare) {
    const auto sort=[&](auto&& self,std::span<std::uint32_t> values)->void {
        for(;;) {
            const auto count=values.size();
            if(count<7) {
                for(std::size_t i=1;i<count;++i)
                    for(auto j=i;j>0&&compare(values[j-1],values[j])>0;--j)
                        std::swap(values[j-1],values[j]);
                return;
            }
            const auto median=[&](std::size_t a,std::size_t b,std::size_t c) {
                if(compare(values[a],values[b])<0) {
                    if(compare(values[b],values[c])<0)return b;
                    if(compare(values[a],values[c])>=0)return a;
                } else {
                    if(compare(values[b],values[c])>0)return b;
                    if(compare(values[a],values[c])<0)return a;
                }
                return c;
            };
            auto pivot=count/2;
            if(count>7) {
                std::size_t first=0,last=count-1;
                if(count>40) {
                    const auto step=count/8;
                    first=median(first,first+step,first+2*step);
                    pivot=median(pivot-step,pivot,pivot+step);
                    last=median(last-2*step,last-step,last);
                }
                pivot=median(first,pivot,last);
            }
            std::swap(values[0],values[pivot]);
            std::size_t equal_left=1,left=1,right=count-1,equal_right=count-1;
            while(left<=right) {
                while(left<=right) {
                    const auto relation=compare(values[left],values[0]);
                    if(relation>0)break;
                    if(relation==0)std::swap(values[equal_left++],values[left]);
                    ++left;
                }
                while(left<=right) {
                    const auto relation=compare(values[right],values[0]);
                    if(relation<0)break;
                    if(relation==0)std::swap(values[right],values[equal_right--]);
                    --right;
                }
                if(left>right)break;
                std::swap(values[left++],values[right--]);
            }
            const auto left_count=left-equal_left,right_count=equal_right-right;
            auto swaps=std::min(equal_left,left_count);
            for(std::size_t i=0;i<swaps;++i)std::swap(values[i],values[left-swaps+i]);
            swaps=std::min(right_count,count-equal_right-1);
            for(std::size_t i=0;i<swaps;++i)std::swap(values[left+i],values[count-swaps+i]);
            // Disjoint partitions and a pure comparator preserve the source permutation
            // whichever is visited first; recurse only into the smaller for O(log N) stack.
            if(left_count<right_count) {
                if(left_count>1)self(self,values.first(left_count));
                if(right_count<=1)return;
                values=values.last(right_count);
            } else {
                if(right_count>1)self(self,values.last(right_count));
                if(left_count<=1)return;
                values=values.first(left_count);
            }
        }
    };
    sort(sort,order);
}

inline bool source_sort_self_test() {
    const auto descending=[](std::uint32_t a,std::uint32_t b){return a>b?-1:a<b?1:0;};
    {
        constexpr std::array<std::uint32_t,6> materials{0,UINT32_MAX,1,UINT32_MAX,0x80000000U,1};
        std::array<std::uint32_t,6> order{0,1,2,3,4,5};
        source_sort_order(order,[&](auto a,auto b){return descending(materials[a],materials[b]);});
        if(order!=std::array<std::uint32_t,6>{1,3,4,2,5,0})return false;
    }
    {
        // Material IDs are serialized unsigned values, not resolved material indices.
        constexpr std::array<std::uint32_t,8> materials{1,2,1,UINT32_MAX,2,1,0x80000000U,2};
        std::array<std::uint32_t,8> order{0,1,2,3,4,5,6,7};
        source_sort_order(order,[&](auto a,auto b){return descending(materials[a],materials[b]);});
        // FUN_00463150's equal-material swaps deliberately differ from a stable sort.
        if(order!=std::array<std::uint32_t,8>{3,6,4,7,1,5,2,0})return false;
    }
    std::array<std::uint32_t,41> ties{};
    for(const std::size_t count:{0U,1U,6U,7U,8U,40U,41U}) {
        std::iota(ties.begin(),ties.end(),0U);
        source_sort_order(std::span(ties).first(count),[](auto,auto){return 0;});
        const auto pivot=count<7?0:count==7?count/2:count-1;
        for(std::size_t i=0;i<count;++i)
            if(ties[i]!=(i==0?pivot:i==pivot?0:i))return false;
    }
    // Bounded adversarial fixture; the decoder retains its existing entry/index budgets.
    constexpr std::size_t count=4096;
    std::array<std::uint32_t,count> order{};
    std::iota(order.begin(),order.end(),0U);
    source_sort_order(order,[](auto,auto){return 0;});
    for(std::size_t i=0;i<count;++i)
        if(order[i]!=(i==0?count-1:i==count-1?0:i))return false;
    std::iota(order.begin(),order.end(),0U);
    const auto material=[](std::uint32_t i){return (i*2654435761U)&1023U;};
    source_sort_order(order,[&](auto a,auto b){return descending(material(a),material(b));});
    std::array<std::uint8_t,count> seen{};
    for(std::size_t i=0;i<count;++i) {
        if(order[i]>=count||seen[order[i]])return false;
        seen[order[i]]=1;
        if(i&&material(order[i-1])<material(order[i]))return false;
    }
    return true;
}
} // namespace ikv::detail
