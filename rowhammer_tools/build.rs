use std::process::Command;

fn main() {
  //-- git hash
  let output = Command::new("git")
    .args(&["log", "-1", "--format=%h"]).output().unwrap();
  let git_hash = String::from_utf8(output.stdout).unwrap();
  println!("cargo:rustc-env=GIT_HASH={}", git_hash);
  //println!("cargo:warning=GIT_HASH={}", git_hash);

}

