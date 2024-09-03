
use dynasmrt::{dynasm, DynasmApi};
use dynasmrt::x64::Assembler;

//Be careful with forward/backward symbol refs, the wrong direction could
//result in less/more iterations!

pub fn push_all_gp_regs(ops: &mut Assembler) {
  dynasm!(ops
    ; push rax
    ; push rbx
    ; push rcx
    ; push rdx
    ; push rdi
    ; push rsi
    ; push r10
    ; push r11
    ; push r12
    ; push r13
    ; push r14
    ; push r15
  );
}

pub fn pop_all_gp_regs(ops: &mut Assembler) {
  dynasm!(ops
    ; pop r15
    ; pop r14
    ; pop r13
    ; pop r12
    ; pop r11
    ; pop r10
    ; pop rsi
    ; pop rdi
    ; pop rdx
    ; pop rcx
    ; pop rbx
    ; pop rax
  );
}

//uses rax
//result in rax
pub fn garbage_add(ops: &mut Assembler, n: u64, garbage_count: u32) {
  for _ in 0..n {
    for _ in 0..garbage_count {
      dynasm!(ops
        ; add rax, 0x1 //data dependence
      );
    }
  }
}
