use std::str::FromStr;
use std::path::Path;
use log::*;

use crate::utils::{self, files, serialize::*};
use crate::config::*;

/*
 * The phys <--> DRAM translation code is very restricted to "normal" Intel
 * systems! "normal" means:
 *   - X = number of bank bits = 2log(#banks * #ranks), (we don't distinguish
 *     between ranks) each bank address bit is determined by 2 phys address
 *     bits (=> each bank fn has 2 ones => 2X bank bits in phys address)
 *   - phys address from LSB to MSB: consecutive column bits, X consecutive
 *     bank bits, consecutive row bits overlapping with another X consecutive
 *     bank bits
 */

#[derive(Debug, Clone)]
pub struct DRAMAddr {
  pub bank: u64,
  pub row: u64,
  pub column: u64,
}

//read memory SPD info
//assumes only 1 DIMM configurations!!
pub fn create_config() -> DRAMConfig {
  info!("Creating DRAM config from {}", DRAM_INFO_PATH);

  let dram_info: DRAMInfo = if Path::new(DRAM_INFO_PATH).exists() {
      files::parse_toml(DRAM_INFO_PATH)
  } else {
    warn!("dram_info.toml not found. Trying to read {}", DRAM_CONFIG_PATH);
    return files::parse_toml(DRAM_CONFIG_PATH);
  };

  let stdout = utils::run(&["decode-dimms"]);

  //this tool currently only supports 1 dimm configs
  let r = &utils::regex(&stdout, r"Decoding EEPROM:");
  assert!(r.len() == 1,
    "There is more than 1 DIMM in the system, this tool only supports 1");

  //dimm assembly serial number, falls back to module manufacturer + part number
  let r_serial_number = utils::regex(&stdout,
    r"Assembly Serial Number\s+(?P<serial>.*)\s*\n").get(0)
    .map(|x| x["serial"].to_owned()).unwrap_or_else(|| {
      let t1 = &utils::regex(&stdout,
        r"Module Manufacturer\s+(?P<manu>.*)\s*\n")[0]["manu"];
      let t2 = &utils::regex(&stdout,
        r"Part Number\s+(?P<part_nr>.*)\s*\n")[0]["part_nr"];
      format!("{} {}", t1, t2).to_owned()
  });

  let dram_id = dram_info.dram_ids.get(&r_serial_number)
    .unwrap_or(&r_serial_number).to_owned();

  //memory type
  let dram_type = &utils::regex(&stdout,
    r"Fundamental Memory type\s+(?P<mem_type>.*)\sSDRAM\s*\n")[0]["mem_type"];
  //TODO check if DDR3 or DDR4

  //banks
  let r_banks = &utils::regex(&stdout,
    r"Banks x Rows x Columns x Bits\s+(?P<banks>\d+)\D.*\n")[0]["banks"];
  let banks = u32::from_str(r_banks).expect("decode-dimms banks NaN");
  assert!(banks.is_power_of_two());

  //ranks
  let r_ranks = &utils::regex(&stdout,
    r"Ranks\s+(?P<ranks>\d+)\s*\n")[0]["ranks"];
  let ranks = u32::from_str(r_ranks).expect("decode-dimms ranks NaN");
  assert!(ranks == 1 || ranks == 2);

  let mapping_functions = dram_info.mapping_functions
    .get(&format!("{}.1.1.{}.{}", dram_type, ranks, banks))
    .expect("No known mapping functions for this DRAM config");

  DRAMConfig {
    dram_id,
    row_fn: mapping_functions.row_fn,
    column_fn: mapping_functions.column_fn,
    bank_fns: mapping_functions.bank_fns.clone(),
  }
}

pub fn phys_to_dram(phys_addr: u64, dram_config: &DRAMConfig) -> DRAMAddr {
  let mut bank: u64 = 0;
  for (i, bank_fn) in dram_config.bank_fns.iter().enumerate() {
    bank |= ((phys_addr & bank_fn).count_ones() as u64 % 2) << i;
  }

  let row: u64 = (phys_addr & dram_config.row_fn) >>
      dram_config.row_fn.trailing_zeros();
  let column: u64 = (phys_addr & dram_config.column_fn) >>
      dram_config.column_fn.trailing_zeros();

  DRAMAddr{bank, row, column}
}

//each bank fn is associated with one bit in the bank number
//col and row fns have one bit set in each fn, bank fns have 2 bits for which
//one overlaps with a row or col fn
pub fn dram_to_phys(dram_addr: &DRAMAddr, dram_config: &DRAMConfig) -> u64 {
  //assumes row and col mask have contiguous ones
  //else, do same as below with bank fn with 1 bit set
  let mut phys: u64 = dram_addr.row << dram_config.row_fn.trailing_zeros();
  phys |= dram_addr.column as u64;

  for (i, bank_fn) in dram_config.bank_fns.iter().enumerate() {
    assert!(bank_fn.count_ones() == 2);
    //there is a row_fn or column_fn bit that also uses one of the bits
    //of this bank fn
    let overlap_mask = (dram_config.row_fn | dram_config.column_fn) & bank_fn;
    assert!(overlap_mask.count_ones() == 1,
      "No overlapping row or column function bit found");
    let non_overlap_mask = overlap_mask ^ bank_fn;
    let bank_addr_bit = (dram_addr.bank >> i) & 1;
    let overlap_bit = (phys & overlap_mask) >> overlap_mask.trailing_zeros();
    let non_overlap_bit = bank_addr_bit ^ overlap_bit;
    phys |= non_overlap_bit << non_overlap_mask.trailing_zeros();
  }

  phys
}
