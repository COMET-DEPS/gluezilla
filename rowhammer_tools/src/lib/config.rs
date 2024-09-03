// --- GENERAL ---
//current commit
pub static GIT_HASH: &str = env!("GIT_HASH");
//hostnames that should not get rowhammered
pub static FORBIDDEN_HOSTNAMES: [&str; 2] = ["name1", "name2"];

// --- SYSTEM CONFIG ---
//2log of the page size
pub static PAGE_SIZE_BITS: u32 = 12;
pub static PAGE_SIZE: usize = 1 << PAGE_SIZE_BITS;
pub static PAGE_OFFSET_MASK: u64 = PAGE_SIZE as u64 - 1;
pub static PAGE_ALIGN_MASK: u64 = !PAGE_OFFSET_MASK;
//bitmask for the present bit of an entry in the pagemap
pub static PRESENT_BITMASK: u64 = 1 << 63;
//bitmask for the swap bit of an entry in the pagemap
pub static SWAP_BITMASK: u64 = 1 << 62;
//number of pages in a row (needs to be power of 2)
pub static PAGES_PER_ROW: u64 = 2;
pub static ROW_SIZE: u64 = PAGES_PER_ROW * PAGE_SIZE as u64;
pub static ROW_ALIGN_MASK: u64 = !(ROW_SIZE - 1);
//path to the config file
pub static ATTACK_CONFIG_PATH: &str = "./attack_config.toml";
//path to the file with victim locations
pub static VICTIM_ADDRESSES_PATH: &str = "./victim_addresses.txt";
//path to the file with templater config
pub static TEMPLATER_CONFIG_PATH: &str = "./templater_config.toml";
//path to the file with dram config
pub static DRAM_CONFIG_PATH: &str = "./dram_config.toml";
//path to the file with dram info
pub static DRAM_INFO_PATH: &str = "./dram_info.toml";
//Booting the system once with a different ram config and then restoring the
//original will change the distribution of true and anti cells compared to the
//previous time the original config was used (apparently...)
//Therefore, for detailed experiments, i have to at least know whether the
//true&anti cell distribution was different compared to another experiment
//There is no way to check for changes in T&A distribution, so i just  keep a
//list of every time i swap the ram and give each entry an id
//This file contains the current id, it is cleared every time the system
//reboots, so i get reminded to recreate the file with either the same id as
//before or the new id if i changed the dimms
pub static RAM_SWAP_ID_FILE: &str = "/tmp/RAM_SWAP_ID";

// --- ARCHITECTURAL CONFIG ---
#[derive(Debug, PartialEq, Eq)]
pub enum UARCH {
  SandyBridge,
  IvyBridge,
  Haswell,
  Skylake, //includes Skylake, Kaby Lake, Coffee Lake, Whiskey Lake, and
           //Comet Lake Intel CPUs
  Alderlake
}

use mmap::MemoryMap;
use std::collections::HashMap;
pub type Frame2Map = HashMap<u64, MemoryMap>;
