// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/random.h>


FUZZ_TARGET(block_template_cache)
{
    // Create a data provider instance
    // Generate a bunch of transactions for lots of weight
    // Create a block template with default size interval n
    // Create a block template with default size with interval n
    // verify the time of creation is the same
    // create a block template with different config
    // verify that we get non identical template
    // advance the chain tip
    // create a template again verify that we get a template.
    // add block validity for all config
    // verify we get the block validity.
    // randomize the block creation options based on input and verfiy we always get a config.
    
}