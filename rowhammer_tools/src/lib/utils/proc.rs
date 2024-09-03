use std::fs::File;
use std::io::{Seek, Read};
use std::path::Path;
use byteorder::{ByteOrder, LittleEndian};

use crate::config::*;

pub fn virt_to_phys(virt_addr: u64) -> u64 {
  let pagemap_offset: u64 = (virt_addr >> 12) * 8;

  let path = Path::new("/proc/self/pagemap");
  let mut pagemap = File::open(path)
    .expect(&format!("Couldn't open {}", path.display()));

  pagemap.seek(std::io::SeekFrom::Start(pagemap_offset))
    .expect("Failed to seek in pagemap");

  //read may fail if page is not present
  let mut buf: [u8; 8] = [0; 8];
  pagemap.read_exact(&mut buf)
    .expect("Failed to read physical address from pagemap");
  let ret = LittleEndian::read_u64(&buf);

  if (ret & PRESENT_BITMASK) == 0 {
    panic!("Virtual to physical address translation failed: page not present");
  }

  if ret & SWAP_BITMASK != 0 {
    panic!("Virtual to physical address translation failed: page is swapped \
      (pages should be locked in memory or swap disabled");
  }

  let page_offset = ((1 << PAGE_SIZE_BITS) - 1) & virt_addr;
  ((ret & ((1 << 55) - 1)) << PAGE_SIZE_BITS) + page_offset
}

/*
use crate::utils::proc;
use std::io::{BufRead, BufReader};
fn put_myself_in_phys_mem() {
  let pattern = "templater";
  let maps =
    File::open("/proc/self/maps").expect("Could not open /proc/self/maps");
  let maps_reader = BufReader::new(maps);

  maps_reader.lines()
    .map(|l| l.unwrap())
    .filter(|l| l.contains(pattern))
    .for_each(|l| {
      let pos1 = l.find("-").expect("Reading /proc/self/maps failed");
      let pos2 = l.find(" ").expect("Reading /proc/self/maps failed");
      let start_addr = u64::from_str_radix(&l[0..pos1], 16).unwrap();
      let end_addr = u64::from_str_radix(&l[pos1+1..pos2],16).unwrap();

      for virt in (start_addr..end_addr).step_by(PAGE_SIZE as usize) {
        let phys1 = proc::virt_to_phys(virt).unwrap_or(0);
        unsafe { std::ptr::read_volatile(virt as *const u8); }
        let phys2 = proc::virt_to_phys(virt).unwrap_or(0);
        assert!(phys2 != 0);
        if phys1 == 0 {
          println!("Succesfully put page 0x{:x} in physical memory", virt);
        }
      }
  });
}
*/
/*
use std::io::{BufReader, BufRead};
//print the maps for arg:path, the new mappings are done by libloader?
//use "/" to print all mappings
pub fn print_phys_addrs_of_maps(path: &str, message: &str) {
  let pattern = &path[(path.rfind('/').unwrap() + 1)..];
  info!("{}", message);
  let maps = File::open("/proc/self/maps")
                  .expect("Could not open /proc/self/maps");
  let maps_reader = BufReader::new(maps);

  for line in maps_reader.lines().map(|l| l.unwrap()) {
    if line.contains(pattern) {
      let pos1 = line.find("-").expect("Reading /proc/self/maps failed");
      let pos2 = line.find(" ").expect("Reading /proc/self/maps failed");
      let start_addr = u64::from_str_radix(&line[0..pos1], 16).unwrap();
      let end_addr = u64::from_str_radix(&line[pos1+1..pos2],16).unwrap();

      for virt in (start_addr..end_addr).step_by(PAGE_SIZE as usize) {
        let phys = virt_to_phys(virt).unwrap_or(0);
        info!("  Virtual address: 0x{:x}, physical address: 0x{:x}", virt, phys);
      }
    }
  }
}
*/
