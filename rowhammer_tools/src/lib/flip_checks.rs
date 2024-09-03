use std::collections::{HashMap, BTreeMap};
use colored::Colorize;
use log::*;

use crate::utils::serialize::*;
use crate::config::*;


//Checks the victims in arg:victim_frames for changes compared to the value in
//arg:content_cache
//Returns true if everthing flipped as expected, false otherwise
pub fn check_victim_flips<'a>(
    victim_frames: impl Iterator<Item = &'a VictimFrame>,
    frame2map: &Frame2Map,
    content_cache: &HashMap<u64, u8>
) -> bool {
  info!("Checking victims for flipped bits");
  let mut expected = true;

  for victim_frame in victim_frames {
    let frame_addr = victim_frame.frame_addr;

    //group to check whole bytes at once for accurate logging
    //page_offset -> [0: no flip, 1: 0->1, -1: 1->0 ; 8]
    let mut bytes = BTreeMap::new(); //to have consistent iter order
    for x in &victim_frame.victim_bits {
      bytes.entry(x.offset)
        .or_insert([0i8; 8])[x.bitflip.flip_index as usize] =
          if x.bitflip.flip_direction {1} else {-1};
    }

    for (page_offset, victim_bits) in bytes {
      let phys_addr = frame_addr + page_offset;
      let virt_addr = (frame2map.get(&frame_addr)
        .unwrap().data() as u64 + page_offset) as *const u8;
      let content_after_rh = unsafe {std::ptr::read_volatile(virt_addr)};
      let content_before_rh = *content_cache.get(&phys_addr).unwrap();

      trace!("  - Content byte before RH: 0x{:x}, after: 0x{:x} (P0x{:x}, V0x{:x})",
        content_before_rh, content_after_rh, phys_addr, virt_addr as u64);

      for flip_idx in 0..8 {
        //both vars contain whether flipped and flip direction (-1, 0, 1)
        let exp_flip = victim_bits[flip_idx];
        let flipped = ((content_after_rh >> flip_idx) & 1) as i8 -
            ((content_before_rh >> flip_idx) & 1) as i8;

        //helper vars
        let exp_sign = if exp_flip == 1 {"+"} else {"-"};
        let sign = if flipped == 1 {"+"} else {"-"};

        if exp_flip != 0 { //we expected a flip
          if flipped == exp_flip { //there was a flip with the same direction
            warn!("{}", format!("Expected bit flip at 0x{:x}: {}{}",
              phys_addr, flip_idx, exp_sign).magenta());
          } else if flipped != 0 && flipped != exp_flip { //flip with diff sign
            warn!("{}", format!("Expected bit flip index, but WRONG sign at \
              0x{:x}: expected {}{}, but found {}{}", phys_addr, flip_idx,
              exp_sign, flip_idx, sign).red());
            expected = false;
          } else { //there was no flip
            error!("{}", format!("Expected bit did NOT flip at 0x{:x}: \
              expected {}{}", phys_addr, flip_idx, exp_sign).red());
            expected = false;
          }
        } else if flipped != 0 { //there was a flip while not expected
          error!("{}", format!("UNexpected bit flip at 0x{:x}: {}{})",
            phys_addr, flip_idx, sign).red());
          expected = false;
        }
      }
    }
  }

  expected
}

//Check the row for changes compared to arg:init_value
//Return the victim info for these flips (phys_addr, bit_idx, flip_sign)
//Flips in the same byte get a separate item in the returned vector
pub fn check_row_for_flips(
  frame2map: &Frame2Map,
  phys_addr: u64,
  init_value: u8
) -> Vec<(u64, BitFlip)> {
  debug!("Checking row P0x{:x} for flips", phys_addr);

  let mut discovered_victims = Vec::new();
  let row_addr = phys_addr % ROW_ALIGN_MASK;

  for phys_addr in row_addr..(row_addr + ROW_SIZE) {
    let victim_frame_offset = phys_addr & PAGE_OFFSET_MASK;
    let virt_addr = (frame2map.get(&(phys_addr & PAGE_ALIGN_MASK))
      .unwrap().data() as u64 + victim_frame_offset) as *const u8;
    let content_after_rh = unsafe {std::ptr::read_volatile(virt_addr)};

    trace!("  - Content byte before RH: 0x{:x}, after: 0x{:x} (P0x{:x}, V0x{:x})",
      init_value, content_after_rh, phys_addr, virt_addr as u64);

    for flip_idx in 0..8 {
      let mask = 1 << flip_idx;
      if init_value & mask != content_after_rh & mask {
        let discovered_victim = (phys_addr, BitFlip {
          flip_index: flip_idx as u8,
          flip_direction: (content_after_rh & mask) != 0
        });
        warn!("{}", format!("Bit flip at 0x{:x}: {}{}", phys_addr, flip_idx,
          if discovered_victim.1.flip_direction {"+"} else {"-"}).red());
        discovered_victims.push(discovered_victim);
      }
    }
  }

  discovered_victims
}
