use std::collections::HashMap;
use log::*;

use crate::utils::{serialize::*, proc};
use crate::config::*;


//Initialize the page at arg:virt_pages with the given init_value
fn initialize_pages(virt_pages: &[u64], init_value: u8) {
  //let init = u64::from_ne_bytes([row.init; 8]);
  for page in virt_pages {
    let page_addr = *page as usize;
    unsafe {
      //assuming cache line size of 64 bytes
      for addr in (page_addr..page_addr + PAGE_SIZE).step_by(64) {
        //if we would use std::ptr::write_bytes (=memset) for the whole page
        //instead of write_volatile for each 64 byte address, we still need
        //to iterate over the addresses to flush each cache line
        std::ptr::write_volatile(addr as *mut [u8; 64], [init_value; 64]);
        core::arch::x86_64::_mm_clflush(addr as *const u8);
      }
    }

    let phys_addr = proc::virt_to_phys(page_addr as u64);
    debug!("Inititialized page V0x{:x}, P0x{:x} with 0x{:x}",
            page_addr, phys_addr, init_value);
  }
}

//Initialize the rows and return a virtual address for each
pub fn initialize_rows<'a>(
  rows: impl Iterator<Item= &'a RowAndInitValue>,
  frame2map: &Frame2Map,
) { //-> Vec<u64> {
  //let mut v = Vec::new();
  for row in rows {
    initialize_pages(&row.frames.iter().map(|frame| {
      frame2map.get(frame).unwrap().data() as u64
    }).collect::<Vec<u64>>(), row.init);
    //only one address in the aggressor row is needed for hammering, take
    //the first byte of the first page
    //v.push(frame2map.get(&row.frames[0]).unwrap().data() as u64);
  }

  //v
}

//TODO is there an influence of the value of the neighbouring bits in the
//victim row on the bitflip behaviour? didn't some paper use this?
//Loader also inits single bit (by loading file)
pub fn initialize_attack_victims(
  attack_config: &AttackConfig, frame2map: &Frame2Map
) {
  for frame in &attack_config.victim_frames {
    let phys_frame = frame.frame_addr;
    let virt_frame = frame2map.get(&phys_frame).unwrap().data() as u64;

    for bitflip in &frame.victim_bits {
      /*
      //init whole "row"
      initialize_rows(std::iter::once(&RowAndInitValue{
        frames: vec![phys_frame], //only one page!! (bcs other not allocated)
        init: if bitflip.bitflip.flip_direction {0x0} else {0xff}
      }), &frame2map);
      */
      //init only victim bit
      let virt_addr = virt_frame + bitflip.offset;
      unsafe {
        let mut init_byte = std::ptr::read_volatile(virt_addr as *const u8);
        let mask: u8 = 1u8 << bitflip.bitflip.flip_index;
        init_byte = if bitflip.bitflip.flip_direction == false
          {init_byte | mask} else {init_byte & !mask};

        std::ptr::write_volatile(virt_addr as *mut u8, init_byte);
        core::arch::x86_64::_mm_clflush(virt_addr as *const u8);

        debug!("Initialized victim V0x{:x} with 0x{:x}", virt_addr, init_byte);
      }
    }
  }
}

//Cache original content to compare with after hammering
//Key is physical address
pub fn read_victim_content<'a>(
  victims: impl Iterator<Item= &'a VictimFrame>,
  frame2map: &Frame2Map
) -> HashMap<u64, u8> {
  info!("Reading the victim contents to compare with later");
  let mut content_cache = HashMap::new();
  for victim in victims {
    let victim_frame = victim.frame_addr;
    for victim_bit in &victim.victim_bits {
      let victim_page = frame2map.get(&victim_frame).unwrap().data() as u64;
      let content = unsafe {std::ptr::read_volatile(
        (victim_page + victim_bit.offset) as *const u8)};
      content_cache.insert(victim_frame + victim_bit.offset, content);
    }
  }

  content_cache
}

