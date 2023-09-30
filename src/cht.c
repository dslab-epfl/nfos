#include "cht.h"
#include <assert.h>
#include <stdlib.h>

static uint64_t loop(uint64_t k, uint64_t capacity)
{
    uint64_t g = k % capacity;
    return g;
}

int nfos_cht_fill_cht(struct NfosVector *cht, uint32_t cht_height, uint32_t backend_capacity)
{
    // Generate the permutations of 0..(cht_height - 1) for each backend
    int *permutations = (int*) malloc(sizeof(int) * (int)(cht_height * backend_capacity));
    if (permutations == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < backend_capacity; ++i)
    {
        uint32_t offset_absolut = i * 31;
        uint64_t offset = loop(offset_absolut, cht_height);
        uint64_t base_shift = loop(i, cht_height - 1);
        uint64_t shift = base_shift + 1;
        for (uint32_t j = 0; j < cht_height; ++j)
        {

            uint64_t permut = loop(offset + shift * j, cht_height);
            permutations[i * cht_height + j] = (int)permut;

        }
    }

    int *next = (int*) malloc(sizeof(int) * (int)(cht_height));
    if (next == 0) {
        free(permutations);
        return 0;
    }

    for (uint32_t i = 0; i < cht_height; ++i)
    {
        next[i] = 0;
    }

    // Fill the priority lists for each hash in [0, cht_height)
    for (uint32_t i = 0; i < cht_height; ++i)
    {
        for (uint32_t j = 0; j < backend_capacity; ++j)
        {
            uint32_t *value;

            uint32_t index = j * cht_height + i;
            int bucket_id = permutations[index];

            int priority = next[bucket_id];

            next[bucket_id] += 1;

            nfos_vector_borrow_unsafe(cht, (int)(backend_capacity * ((uint32_t)bucket_id) + ((uint32_t)priority)), (void **)&value);
            *value = j;
        }
    }

    // Free memory
    free(next);
    free(permutations);
    return 1;
}

int nfos_cht_find_preferred_available_backend(uint64_t hash, struct NfosVector *cht, struct NfosDoubleChainExp *active_backends, uint32_t cht_height, uint32_t backend_capacity, int *chosen_backend)
{
    uint64_t start = loop(hash, cht_height);
    for (uint32_t i = 0; i < backend_capacity; ++i)
    {
        uint64_t candidate_idx = start * backend_capacity + i;

        uint32_t *candidate;
        nfos_vector_borrow(cht, (int)candidate_idx, (void **)&candidate);

        if (nfos_dchain_exp_is_index_allocated(active_backends, (int)*candidate)) {
            *chosen_backend = (int)*candidate;

            return 1;
        }

    }
    return 0;
}
