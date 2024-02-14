// ethash: C/C++ implementation of Ethash, the Ethereum Proof of Work algorithm.
// Copyright 2018-2019 Pawel Bylica.
// Licensed under the Apache License, Version 2.0.

// Modified by Firominer's authors 2021

#pragma once
#ifndef CRYPTO_ETHASH_HPP_
#define CRYPTO_ETHASH_HPP_

#include <memory>
#include <optional>

#include <intx/intx.hpp>

#include "attributes.hpp"
#include "keccak.hpp"

namespace ethash
{
// Internal constants:
constexpr static uint32_t kRevision = 23;

/**
 * Ethereum epoch is 30000 blocks which, with an avg 13 sec block time, corresponds
 * to roughly 108 hours i.e. 4.5 days. To achieve the same DAG growth rate with
 * a block time of 5 min we need to set epoch length to 30000/(300/13) which
 * provides a DAG increase every 1300 blocks i.e. 4.51 days
 * See "./lib/ethash/ethash.cpp" for increase size(s)
 */

constexpr static uint32_t kEpoch_length = 7500;  // Meowpow

constexpr static uint32_t kLight_cache_item_size = 64;
constexpr static uint32_t kFull_dataset_item_size = 128;
constexpr static uint32_t kNum_dataset_accesses = 64;
constexpr static uint32_t kLight_cache_init_size = 1 << 24;
constexpr static uint32_t kLight_cache_growth = 1 << 17;
constexpr static uint32_t kLight_cache_rounds = 3;
constexpr static uint32_t kL1_cache_size = 16 * 1024;
constexpr static uint32_t kL1_cache_words = kL1_cache_size / sizeof(uint32_t);
constexpr static uint32_t kFull_dataset_init_size = (1U << 30);
constexpr static uint32_t kFull_dataset_growth = 1 << 23;
constexpr static uint32_t kFull_dataset_item_parents = 512;

struct epoch_context
{
    const uint32_t epoch_number;
    const uint32_t light_cache_num_items;
    const size_t light_cache_size;
    const uint32_t full_dataset_num_items;
    const size_t full_dataset_size;
    const hash512* const light_cache;
    const uint32_t* const l1_cache;
    hash1024* full_dataset;
};


struct result
{
    hash256 final_hash;
    hash256 mix_hash;
};

enum class VerificationResult
{
    kOk,             // Verification ok
    kInvalidNonce,   // Produces a hash above target
    kInvalidMixHash  // Provided mix_hash does not match with computed
};

namespace detail
{
// using lookup_fn = hash1024 (*)(const epoch_context&, uint32_t);
using hash_512_function = hash512 (*)(const uint8_t* data, size_t size);

hash1024 lazy_lookup_1024(const epoch_context& context, uint32_t index) noexcept;
hash2048 lazy_lookup_2048(const epoch_context& context, uint32_t index) noexcept;
hash1024 calculate_dataset_item_1024(const epoch_context& context, uint32_t index) noexcept;
hash2048 calculate_dataset_item_2048(const epoch_context& context, uint32_t index) noexcept;

hash512 hash_seed(const hash256& header, uint64_t nonce) noexcept;
hash256 hash_mix(const epoch_context& context, const hash512& seed);
hash256 hash_final(const hash512& seed, const hash256& mix) noexcept;

void destroy_epoch_context(epoch_context* context) noexcept;

/**
 * Creates the dag epoch context
 */
epoch_context* create_epoch_context(uint32_t epoch_number, bool full);

}  // namespace detail

/**
 * Finds the largest prime number not greater than the provided upper bound.
 *
 * @param upper_bound  The upper bound.
 * @return  The largest prime number `p` such `p <= upper_bound`.
 *          In case `upper_bound <= 1`, returns 0.
 */
uint32_t find_largest_unsigned_prime(uint32_t upper_bound) noexcept;

/**
 * Calculates the number of items in the light cache for given epoch.
 *
 * This function will search for a prime number matching the criteria given
 * by the Ethash so the execution time is not constant. It takes ~ 0.01 ms.
 *
 * @param epoch_number  The epoch number.
 * @return              The number items in the light cache.
 */
uint32_t calculate_light_cache_num_items(uint32_t epoch_number) noexcept;

/**
 * Calculates the number of items in the full dataset for given epoch.
 *
 * This function will search for a prime number matching the criteria given
 * by the Ethash so the execution time is not constant. It takes ~ 0.05 ms.
 *
 * @param epoch_number  The epoch number.
 * @return              The number items in the full dataset.
 */
uint32_t calculate_full_dataset_num_items(uint32_t epoch_number) noexcept;

size_t get_light_cache_size(int num_items) noexcept;
size_t get_full_dataset_size(int num_items) noexcept;

/**
 * Calculates the epoch seed hash.
 * @param epoch_number  The epoch number.
 * @return              The epoch seed hash.
 */
hash256 calculate_seed_from_epoch(uint32_t epoch_number) noexcept;

/**
 * Calculates the epoch number provided a seed hash.
 * @param seed          The hash256 seed
 * @return              The epoch number if found.
 */
std::optional<uint32_t> calculate_epoch_from_seed(const hash256& seed) noexcept;

/**
 * Calculates the epoch number provided a block number.
 * @param block_num     The block number
 * @return              The epoch number
 */
uint32_t calculate_epoch_from_block_num(const uint64_t block_num) noexcept;

/**
 * Performs a full ethash round with given nonce
 * @param context       The DAG epoch context.
 * @param header        The header hash of the block to be hashed
 *
 * @param nonce         The nonce to use
 * @return              A result struct holding both the final hash and the mix hash
 */
result hash(const epoch_context& context, const hash256& header, uint64_t nonce);

/**
 * Verifies only the final hash provided a header hash and a mix hash
 * It does not traverse the memory hard part and
 * assumes mix_hash is valid
 * @param header_hash
 * @param mix_hash
 * @param nonce
 * @param boundary
 * @return              True / False
 */
bool verify_light(
    const hash256& header_hash, const hash256& mix_hash, uint64_t nonce, const hash256& boundary) noexcept;

/**
 * Verifies the whole ethash outcome validating mix_hash and final_hash againts
 * the boundary. It does traverse the memory hard part
 * @param header_hash
 * @param mix_hash
 * @param nonce
 * @param boundary
 * @return              True / False
 */
VerificationResult verify_full(const epoch_context& context, const hash256& header_hash, const hash256& mix_hash,
    uint64_t nonce, const hash256& boundary) noexcept;

/**
 * Verifies the whole ethash outcome validating mix_hash and final_hash againts
 * the boundary. It does traverse the memory hard part
 * @param header_hash
 * @param mix_hash
 * @param nonce
 * @param boundary
 * @return              True / False
 */
VerificationResult verify_full(const uint64_t block_num, const hash256& header_hash, const hash256& mix_hash,
    uint64_t nonce, const hash256& boundary) noexcept;

using epoch_context_ptr = std::unique_ptr<epoch_context, decltype(&detail::destroy_epoch_context)>;

/**
 * Creates an DAG context for given epoch number
 * @param epoch_number
 * @return              A unique_ptr to the context
 */
std::shared_ptr<epoch_context> get_epoch_context(uint32_t epoch_number, bool full) noexcept;

hash256 get_boundary_from_diff(const intx::uint256 difficulty) noexcept;

hash256 from_bytes(const uint8_t* data);

}  // namespace ethash


#endif  // !CRYPTO_ETHASH_HPP_
