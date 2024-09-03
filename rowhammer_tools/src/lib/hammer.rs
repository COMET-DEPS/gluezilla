use std::env;
use std::time::{SystemTime, Duration};
use std::fs::File;
use std::io::Write;
use std::arch::asm;
use dynasmrt::x64::Assembler;
use log::*;
use dynasmrt::{dynasm, DynasmApi, DynasmLabelApi};
use capstone::prelude::*;

use crate::utils::garbage::*;

//Different rowhammer implementations, each hammers one aggressor pattern
//for one victim

//Hammer with Rust
//(checked: accesses and clflushes are not removed during
//optimization in release build)
pub fn hammer_rust(pattern: &Vec<u64>, hammer_count: u64) -> Duration {
  let start_time = SystemTime::now();
  for _ in 0..hammer_count {
    unsafe {
      for addr in pattern {
        std::ptr::read_volatile(*addr as *const u32);
      }
      for addr in pattern {
        core::arch::x86_64::_mm_clflush(*addr as *const u8);
      }
    }
  }

  let duration = SystemTime::now().duration_since(start_time).unwrap();
  debug!("Hammering took {}ms", duration.as_millis());
  duration
}

//Hammer with assembly with garbage interleaving
pub fn hammer_asm(pattern: &Vec<u64>, gar: u32, hammer_count: u64) -> Duration {
  //otherwise, infinite loop
  assert!(gar != 0, "garbage count should not be 0 for asm hammering");

  //if adds are dependent of load
  /*
  for d in virt_addrs {
    unsafe {
      *(*d as *mut i64) = -(gar as i64);
    }
  }
  */

  let start_time = SystemTime::now();

  //intel syntax!
  unsafe {asm!(
    //"lea     0xf(%rip),{ee}",
    "2:",
    //reset aggr_base and itr
    "mov     {b}, {aggr_base}",
    "mov     {i}, {itr}",

    //load aggrs
    "3:",
    "mov     {aggr}, [{b}]",
    "mov     {garbage}, [{aggr}]", //aggr access
    //"clflush [{aggr}]", //flush aggrs

    //slowdown hammering
    //with add instructions
    "mov     {garbage}, {r}", //if adds are independent of load
    "5:",
    "add     {garbage}, 0x1",
    //"clflush [{ee}]",
    "jne     5b",
    //increment to next aggr
    "add     {b}, 0x8",
    "inc     {i}",
    "jne     3b",

    //flush aggrs
    "mov     {b}, {aggr_base}",
    "mov     {i}, {itr}",
    "4:",
    "mov     {aggr}, [{b}]",
    "clflush [{aggr}]", //flush aggrs
    //increment to next aggr
    "add     {b}, 0x8",
    "inc     {i}",
    "jne     4b",

    "inc     {cnt}", //inc hammer count
    "jne     2b",

    //uses 64bit regs by default
    cnt = inout(reg) -(hammer_count as i64) => _, //in and clobbered
    aggr_base = in(reg) pattern.as_ptr() as *const u64,
    b = out(reg) _,
    itr = in(reg) -(pattern.len() as i64),
    i = out(reg) _,
    garbage = out(reg) _,
    aggr = out(reg) _,
    r = in(reg) -(gar as i64),
    //ee = out(reg) _
  );}

  let duration = SystemTime::now().duration_since(start_time).unwrap();
  debug!("Hammering took {}ms", duration.as_millis());
  duration
}

pub extern "C" fn print(fmt: *const i8, arg: u64) {
  unsafe {libc::printf(fmt, arg);}
}

pub extern "C" fn sleep(millis: u64) {
  std::thread::sleep(std::time::Duration::from_millis(millis));
}

/*
    //asm print
    ; lea rdi, [->int_format]
    ; mov rsi, r12
    ; mov rax, QWORD print as _
    ; call rax
    ; mov rdi, 1
    ; mov rax, QWORD sleep as _
    ; call rax
*/


//This function dynamically creates the hammer code in an unrolled hammer loop
//CAREFUL not to clobber the regs used for aggr addresses!
pub fn create_hammer_jit(
  ops: &mut Assembler,
  pattern: &Vec<u64>,
  hammer_count: u64,
  #[allow(unused_variables)]
  garbage_fn: Box<dyn Fn(&mut Assembler)>
) {

  dynasm!(ops
    ; mov r12, QWORD pattern[0] as i64
    ; mov r13, QWORD pattern[1] as i64
  );

  //2
  for _ in 0..hammer_count {
    dynasm!(ops
      ; mov rdx, [r12]
      ; mov rdx, [r13]
      ; clflush [r12]
      ; clflush [r13]
    );
    garbage_fn(ops);
  }
}

//Hammer with dynamically generated hammer code in an unrolled hammer loop,
//with garbage interleaving
pub fn hammer_jit(
  pattern: &Vec<u64>,
  garbage_count: u32,
  hammer_count: u64
) -> Duration {
  debug!("JITing the rowhammer code");

  let mut ops = dynasmrt::x64::Assembler::new().unwrap();
  let code = ops.offset();
  dynasm!(ops
    ; .arch x64
    ; ->int_format:
    ; .bytes "%lx\n\0".as_bytes()
  );

  push_all_gp_regs(&mut ops);
  create_hammer_jit(&mut ops, pattern, hammer_count, Box::new(move |mut ops| {
    garbage_add(&mut ops, 1, garbage_count);
  }));
  pop_all_gp_regs(&mut ops);
  dynasm!(ops; ret);

  let buf = ops.finalize().unwrap();
  let hammer: extern fn() = unsafe {
    std::mem::transmute(buf.ptr(code))
  };

  //if the env variable `JIT_DUMP` is set, dump the jitted code to a file
  //iterate in chunks to prevent out-of-memory error for very big code regions
  //TODO the chunks could split instructions apart! => wrong disassembly and
  //restart at address 0x0
  //TODO change so it only dumps one iteration, create file overwrites the
  //previous iteration
  //for now, use kill to stop the process
  if env::var("JIT_DUMP").is_ok() {
    let chunk_size = 1_000_000;
    warn!("Dumping JIT code to file, expecting about {} iterations",
      buf.len() / chunk_size);
    let mut file = File::create("disassembly_jit_code.txt").unwrap();
    let cs = Capstone::new().x86().mode(arch::x86::ArchMode::Mode64)
      .build().expect("Failed to create Capstone object");
    for (itr, chunk) in buf[..].chunks(chunk_size).enumerate() {
      warn!("dumping iteration {}", itr);
      let insns = cs.disasm_all(chunk, 0x0).expect("Failed to disassemble");
      for insn in insns.as_ref() {
        write!(file, "{}\n", insn).unwrap();
      }
    }
    warn!("Dumping JIT code to file finished");
  }

  //run the jitted code
  debug!("Executing JITed rowhammer code");
  let start_time = SystemTime::now();
  hammer();
  let duration = SystemTime::now().duration_since(start_time).unwrap();
  debug!("Hammering took {}ms", duration.as_millis());

  duration
}

