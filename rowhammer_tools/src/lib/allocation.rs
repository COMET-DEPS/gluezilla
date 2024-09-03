
use std::fs::File;
use std::os::unix::io::AsRawFd;
use std::collections::HashSet;
use std::collections::HashMap;
use mmap::{MemoryMap,MapOption};
use log::*;

use crate::utils::{self, serialize::*, dram, proc};
use crate::config::*;


//// MAPPING ///////////////////////////////////////////////////////////////////

//Reads the target binary and maps the pages of the PT_NULL segment
//then remaps them into a contiguous virtual address space starting
//at the address provided in the attack_config.toml file
pub fn map_binary(
  program_path: &str, attack_config: &AttackConfig,
  frame2map: &mut Frame2Map
) {
  info!("Mapping binary file {}", program_path);

  let file = File::open(&program_path).expect("Could not open file");
  let elf_file =
    elf::File::open_path(program_path).expect("Open ELF file failed");
  let segment = elf_file.phdrs.iter()
    .find(|&&i| i.progtype == elf::types::PT_NULL)
    .expect("Could not find PT_NULL segment");

  let segment_file_offset = segment.offset;
  let segment_page_count = (segment.filesz >> PAGE_SIZE_BITS) + 1;
  info!("The PT_NULL segment contains {} pages", segment_page_count);
  //the filesz and memsz should be the same for segment that contains only code
  assert!(segment_page_count == (segment.memsz >> PAGE_SIZE_BITS) + 1,
    "memsz != filesz, does your section contain only code?");

  //to keep the order of the pages in PT_NULL segment
  let mut segment_pages = Vec::new();

  //1. map the whole segment in random pages
  debug!("mapping:");
  for page_index in  0..segment_page_count {
    let page_file_offset = segment_file_offset + page_index * PAGE_SIZE as u64;
    let page = MemoryMap::new(PAGE_SIZE as usize,
      &[MapOption::MapReadable, MapOption::MapExecutable,
        MapOption::MapFd(file.as_raw_fd()),
        MapOption::MapOffset(page_file_offset as usize)])
      .expect("Could not map page from PT_NULL segment");

    //read to put in physical memory
    unsafe {std::ptr::read_volatile(page.data() as *const u8);}

    debug!("  - PT_NULL segment page {} (file offset 0x{:x}) at V0x{:x}",
      page_index, page_file_offset, page.data() as u64);

    let frame_addr = proc::virt_to_phys(page.data() as u64);
    segment_pages.push(frame_addr);
    frame2map.insert(frame_addr, page); //keep ownership together in frame2map
  }

  //2. memcpy the pages to the required page frame
  debug!("memcpying:");
  for victim_frame in &attack_config.victim_frames {
    let page_index =
      ((victim_frame.page_file_offset.unwrap() - segment_file_offset)
      >> PAGE_SIZE_BITS) as usize;
    let src_page = frame2map.remove(&segment_pages[page_index]).unwrap();
    //dest is already allocated earlier
    let dest_page = frame2map.get(&victim_frame.frame_addr).unwrap().data();
    debug!("  - segment page {} to V0x{:x} (P0x{:x})",
      page_index, src_page.data() as u64, dest_page as u64);
    unsafe {
      let dst = std::slice::from_raw_parts_mut(dest_page, PAGE_SIZE);
      let src = std::slice::from_raw_parts(src_page.data(), PAGE_SIZE);
      dst.copy_from_slice(src);
    }

    let tmp = proc::virt_to_phys(dest_page as u64);
    assert!(victim_frame.frame_addr == tmp, "page frame changed after memcpy");

    segment_pages[page_index] = victim_frame.frame_addr;
    //`src_page` goes out of scope, the mapping is dropped
  }

  //3. remap the segment pages to a contiguous area at a fixed address
  let mut virt_addrs: HashMap<u64, u64> =
    frame2map.iter().map(|(k,v)| (v.data() as u64, *k)).collect();
  debug!("mremapping:");
  for (page_index, frame_addr) in segment_pages.iter().enumerate() {
    let old_page_addr = frame2map[&frame_addr].data() as u64;
    let new_page_addr =
      attack_config.segment_virt_addr + (page_index * PAGE_SIZE) as u64;
    //to prevent libc::mremap from unmapping any other (aggressor) page that
    //was already mapped at the new address
    if let Some(v) = virt_addrs.remove(&new_page_addr) {
      //allocate and deallocate a random page to get a virtual address that is
      //unmapped for sure
      let page = MemoryMap::new(PAGE_SIZE as usize, &[MapOption::MapReadable])
        .expect("Page allocation failed");
      let addr = page.data();
      std::mem::drop(page);
      frame2map.get_mut(&v).unwrap().mremap(addr);
      virt_addrs.insert(addr as u64, v);
    }

    frame2map.get_mut(&frame_addr).unwrap().mremap(new_page_addr as *mut u8);
    assert!(*frame_addr == proc::virt_to_phys(new_page_addr),
      "Physical address changed during mremap!");
    debug!("  - V0x{:x} -> V0x{:x} (P0x{:x})",
      old_page_addr, new_page_addr, frame_addr);
  }
}


//// ALLOCATIONS ///////////////////////////////////////////////////////////////

//Allocates all pages whose frame address is in arg:frames_to_allocate
//see `drop_frac` in docs/example_templater_config.toml for arg:drop_frac
//Return these pages (with ownership) and there phys addr
pub fn allocate_pages(
  mut frames_to_allocate: HashSet<u64>, drop_frac: f64
) -> Frame2Map {
  let limit = (drop_frac * frames_to_allocate.len() as f64) as usize;
  info!("Looking for {} frames, allows {}% loss (= {} frames)",
    frames_to_allocate.len(), drop_frac * 100f64, limit);
  for item in &frames_to_allocate {
    trace!("  - 0x{:x}", item);
  }

  //[MemoryMap] for all allocated pages of no interest (= does not contain
  //victims or are part of an aggressor row)
  let mut garbage_pages = Vec::new();
  //[u64 -> MemoryMap] for all allocated wanted pages
  let mut frame2map = HashMap::new();

  //start allocating
  let mut counter = 1u64;
  while frames_to_allocate.len() > limit {
    let (frame_addr, page_addr, page) = allocate_page();
    if !frames_to_allocate.remove(&frame_addr) {
      garbage_pages.push(page);
      trace!("Frame is not needed");
    } else {
      info!("{}", format!("{}. Found frame P0x{:x} (page V0x{:x})",
        counter, frame_addr, page_addr));
      frame2map.insert(frame_addr, page);
      counter += 1;
    }
  }

  //release the system from the memory stress
  info!("Unmap all pages of no interest");
  std::mem::drop(garbage_pages);
  //this seems to increase RH success
  utils::clear_page_cache();

  frame2map
}

//Allocate read+write+private+anonymous page and access it to put it in
//physical memory
//Return (physical address, virtual address, MemoryMap)
fn allocate_page() -> (u64, u64, MemoryMap) {
  //default MAP_PRIVATE and MAP_ANONYMOUS
  let page = MemoryMap::new(
    PAGE_SIZE as usize,
    &[MapOption::MapReadable, MapOption::MapExecutable, MapOption::MapWritable]
  ).expect("Page allocation failed");
  let page_addr = page.data();

  //Access the page so it is placed in physical memory
  //(can use MAP_POPULATE in mmap for this)
  //Write some value on the page to trigger copy on write
  //(even though i didn't test with page dedupliction turned on)
  unsafe {std::ptr::write(page_addr, 0);}

  let frame_addr = proc::virt_to_phys(page_addr as u64);

  trace!("Allocated page V0x{:x} (P0x{:x})", page_addr as u64, frame_addr);

  (frame_addr, page_addr as u64, page)
}

//Allocates pages until all victims and aggressors are found
//Returns a map: frame addr -> MemoryMap objects (that have ownership over
//the allocated pages) for victims and aggressors
pub fn allocate_attack (
  dram_config: &DRAMConfig,
  attack_config: &AttackConfig
) -> Frame2Map {
  info!("Allocating pages while looking for aggressors and victims");

  //all the frames we need to allocate (victims + aggressors)
  let mut frames_to_allocate: HashSet<u64> = HashSet::new();
  //add victim
  frames_to_allocate.extend(
    attack_config.victim_frames.iter().map(|x| x.frame_addr));
  //add aggressors
  frames_to_allocate.extend(attack_config.iter_aggr_frames());

  //frame2map (frame addr -> MemoryMap) keeps ownership of the MemoryMap object
  //and thus keeps the pages allocated
  let frame2map = allocate_pages(frames_to_allocate, 0f64);

  //attack_config is validated so there should be no overlap between
  //victim and aggressor rows
  attack_config.victim_frames.iter().for_each(|x| {
    debug!("victim: 0x{:x} -> {:?}",
      x.frame_addr, dram::phys_to_dram(x.frame_addr, dram_config));
  });

  attack_config.iter_aggr_frames().for_each(|x| {
    debug!("aggressor: 0x{:x} (for row 0x{:x}) -> {:?}",
      x, x & ROW_ALIGN_MASK, dram::phys_to_dram(x, dram_config));
  });

  frame2map
}

