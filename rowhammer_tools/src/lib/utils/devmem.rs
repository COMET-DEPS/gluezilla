use std::fs::OpenOptions;
use std::io::{Seek, Write, Read};
use log::info;

use crate::utils::serialize::*;
use crate::config::*;

//For these functions to work, either:
//  - the running kernel is compiled with CONFIG_STRICT_DEVMEM=n (the `nopat`
//    commandline argument is not required)
//  - the PTEditor kernel module is loaded
//    (https://github.com/misc0110/PTEditor)


//Fakes the rowhammer effect by flipping bits using /dev/mem
//This function does flip all victims at once
pub fn hammer_fake(
  attack_config: &AttackConfig, frame2map: &Frame2Map
) {
  info!("Performing fake RowHammer with /dev/mem");
  let mut devmem = OpenOptions::new()
    .read(true).write(true).open("/dev/mem").expect("Could not open /dev/mem");
  //TODO add O_SYNC
  for victim_frame in &attack_config.victim_frames {
    let victim_addr = victim_frame.frame_addr;
    for junction_bit in &victim_frame.victim_bits {
      let expected_flip = &junction_bit.bitflip;
      let virt_victim = frame2map.get(&victim_addr)
        .unwrap().data() as u64 + junction_bit.offset;
      let phys_victim = victim_addr + junction_bit.offset;

      //flush the victim so the next access is done from memory
      unsafe {core::arch::x86_64::_mm_clflush(virt_victim as *mut u8);}

      //read the original value
      let mut value = [0; 1];
      devmem.seek(std::io::SeekFrom::Start(phys_victim))
        .expect("Failed to seek in pagemap");
      devmem.read_exact(&mut value).unwrap();

      //apply the bitflip
      let mask: u8 = 1u8 << expected_flip.flip_index;
      value[0] = if expected_flip.flip_direction == true {
        value[0] | mask
      } else {
        value[0] & !mask
      };

      //write the change to memory
      devmem.seek(std::io::SeekFrom::Start(phys_victim))
        .expect("Failed to seek in pagemap");
      devmem.write_all(&value).unwrap();
    }
  }
  // /dev/mem map drops
}
