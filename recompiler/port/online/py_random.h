// py_random.h - CPython's random.Random (MT19937) for a text seed: the same numbers and shuffle(), so the
// SpotPass rotation built here matches tools/boss/make_vssetting.py on every PC.
#pragma once
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace rbrew_native {

class PyRandom {
public:
    explicit PyRandom(std::string_view seed); // random.Random(seed) for a str seed (version 2)
    uint32_t next32();                        // genrand_uint32, getrandbits(32)
    uint32_t getrandbits(int bits);           // 1..32
    uint32_t randbelow(uint32_t n);           // _randbelow_with_getrandbits

    template <typename T> void shuffle(std::vector<T>& items) {
        for (size_t i = items.size(); i-- > 1;) std::swap(items[i], items[randbelow(uint32_t(i + 1))]);
    }

private:
    void init_by_array(const std::vector<uint32_t>& key);
    uint32_t state_[624];
    int index_ = 625;
};

} // namespace rbrew_native
