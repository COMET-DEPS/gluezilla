use std::collections::HashSet;
use std::str::FromStr;
use std::fs;
use log::*;

use crate::config::*;
use crate::utils::{self, serialize::*};


pub fn get_motherboard_id() -> String {
  //motherboard serial number, falls back to product name
  let stdout = utils::run(&["dmidecode", "-t 2"]);
  let motherboard_id = match utils::regex(&stdout,
    r"\s*Serial Number:\s*(?P<serial>.*)\s*\n")[0]["serial"].as_ref() {
    "" | "Default string" => {
      utils::regex(&stdout,
        r"\s*Product Name:\s*(?P<name>.*)\s*\n")[0]["name"].to_owned()
    },
    t => t.to_owned()
  };

  motherboard_id
}

pub fn get_cpu_model() -> String {
  let stdout = utils::run(&["lscpu"]);
  utils::regex(&stdout,
    r"\s*Model name:\s*(?P<name>.*)\s*\n")[0]["name"].to_owned()
}

pub fn check_turbo_boost_disabled() -> bool {
  let stdout = utils::run(&["rdmsr", "0x1a0", "-f", "38:38"]);
  let t = u32::from_str(stdout.trim()).expect("rdmsr turbo boost value NaN");
  assert!(t == 0 || t == 1);
  let turbo_disabled = t == 1;
  if !turbo_disabled {
    warn!("Turbo boost is NOT disabled! Enter anything to continue");
    std::io::stdin().read_line(&mut String::new()).unwrap();
  }

  turbo_disabled
}

pub fn check_smt_disabled() -> bool {
  let smt = fs::read_to_string("/sys/devices/system/cpu/smt/active").unwrap();
  let smt_disabled = match smt.trim() {
    "1" => {
      warn!("SMT is NOT disabled! Enter anything to continue");
      std::io::stdin().read_line(&mut String::new()).unwrap();
      false
    },
    "0" => true,
    _ => panic!("Cannot parse contents of /sys/devices/system/cpu/smt/control")
  };

  smt_disabled
}

pub fn get_assigned_cpu_cores() -> HashSet<u32> {
  //get assigned cores with `taskset -p <own_pid>`
  let pid = std::process::id();
  let stdout = utils::run(&["taskset", "-p", &pid.to_string()]);
  let m = &utils::regex(&stdout, r".*:\s*(?P<mask>.*)\s*\n")[0]["mask"];
  let mask = u32::from_str_radix(m, 16).expect("taskset cpu mask NaN");
  //assume at most 32 cores
  (0..32).filter(|i| mask & (1 << i) != 0).collect()
}

pub fn get_isolated_cpu_cores() -> HashSet<u32> {
  //get all isolated cores from /sys/devices/system/cpu/isolated
  let isolated =
    fs::read_to_string("/sys/devices/system/cpu/isolated").unwrap();
  parse_sys_cpu_format(&isolated)
}

pub fn get_all_cpu_cores() -> HashSet<u32> {
  //get all isolated cores from /sys/devices/system/cpu/isolated
  let online = fs::read_to_string("/sys/devices/system/cpu/online").unwrap();
  parse_sys_cpu_format(&online)
}

fn parse_sys_cpu_format(sys_cpu: &str) -> HashSet<u32> {
  //format example: 1,5-8
  sys_cpu.trim().split(",").map(|x| {
    match &x.split("-").collect::<Vec<_>>()[..] {
      [c] if c.is_empty() =>
        Box::new(std::iter::empty()) as Box<dyn Iterator<Item = u32>>,
      [c] => Box::new(std::iter::once(u32::from_str(c)
        .expect("isolated core id NaN"))) as Box<dyn Iterator<Item = u32>>,
      [c1, c2] => {
        let i1 = u32::from_str(c1).expect("isolated core id NaN");
        let i2 = u32::from_str(c2).expect("isolated core id NaN");
        Box::new(i1..i2) as Box<dyn Iterator<Item = u32>>
      },
      _ => panic!("Cannot parse contents of /sys/devices/system/cpu/isolated")
    }
  }).flatten().collect()
}

pub fn check_cpu_cores_isolated(cores_info: &mut Vec<(u32, Vec<String>)>) {
  let isolated_cores = get_isolated_cpu_cores();
  cores_info.iter_mut().for_each(|(core, info)| {
    //check if the assigned cores are isolated
    let is_isolated = isolated_cores.contains(core);
    if !is_isolated {
      warn!("{}", format!("Core {} is NOT isolated! \
        Enter anything to continue", core));
      std::io::stdin().read_line(&mut String::new()).unwrap();
    }
    info.push(format!("is_isolated: {}", is_isolated));
  });
}

pub fn check_cpu_scaling_governer(cores_info: &mut Vec<(u32, Vec<String>)>) {
  cores_info.iter_mut().for_each(|(core, info)| {
    //check scaling governor of assigned cores
    let governor = fs::read_to_string(&format!(
      "/sys/devices/system/cpu/cpu{}/cpufreq/scaling_governor", core)).unwrap();
    if governor.trim() != "performance" {
      warn!("{}", format!("Scaling governor for core {} is NOT 'performance'! \
        Enter anything to continue", core));
      std::io::stdin().read_line(&mut String::new()).unwrap();
    }
    info.push(format!("scaling_governor: {}", governor));
  });
}

pub fn check_cpu_freq_maxed(cores_info: &mut Vec<(u32, Vec<String>)>) {
  cores_info.iter_mut().for_each(|(core, info)| {
    //check frequency of assigned cores
    let hw_max_freq = fs::read_to_string(&format!(
      "/sys/devices/system/cpu/cpu{}/cpufreq/cpuinfo_max_freq", core)).unwrap();
    let max_freq = fs::read_to_string(&format!(
      "/sys/devices/system/cpu/cpu{}/cpufreq/scaling_max_freq", core)).unwrap();
    let min_freq = fs::read_to_string(&format!(
      "/sys/devices/system/cpu/cpu{}/cpufreq/scaling_min_freq", core)).unwrap();

    let all_hw_max = max_freq.trim() == hw_max_freq.trim()
      && min_freq.trim() == hw_max_freq.trim();
    if !all_hw_max {
      warn!("{}", format!("Min & max frequency for core {} is NOT the \
        hardware max frequency! Enter anything to continue", core));
      std::io::stdin().read_line(&mut String::new()).unwrap();
    }
    info.push(format!("Frequencies: all_hw_max: {} (hw max: {}kHz, \
      max: {}kHz, min: {}kHz)", all_hw_max, hw_max_freq, max_freq, min_freq));
  });
}

pub fn get_os_info() -> (String, String) {
  //read os-release
  let os = fs::read_to_string("/etc/os-release").unwrap();
  let os_release = format!("{} {}",
    &utils::regex(&os, "^NAME=\"(.*)\"")[0][1],
    //flag 'm' enables multiline mode (= treat every line as string)
    &utils::regex(&os, "(?m)^VERSION=\"(.*)\"")[0][1]);

  let kernel = format!("{} {}",
    &utils::run(&["uname", "-s"]).trim(),
    &utils::run(&["uname", "-r"]).trim());

  (os_release, kernel)
}

pub fn get_ram_swap_id() -> u32 {
  fs::read_to_string(RAM_SWAP_ID_FILE)
    .expect(&format!("RAM_SWAP_ID_FILE: {} not found", RAM_SWAP_ID_FILE))
    .trim().parse().unwrap()
}

pub fn read_config() -> HostConfig {
  info!("Creating host config");

  let motherboard_id = get_motherboard_id();
  let cpu_model = get_cpu_model();
  let turbo_disabled = check_turbo_boost_disabled();
  let smt_disabled = check_smt_disabled();
  let mut cores_info =
    get_assigned_cpu_cores().iter().map(|x| (*x, Vec::new())).collect();
  check_cpu_cores_isolated(&mut cores_info);
  check_cpu_scaling_governer(&mut cores_info);
  check_cpu_freq_maxed(&mut cores_info);
  let (os_release, kernel) = get_os_info();
  let ram_swap_id = get_ram_swap_id();

  HostConfig {
    hostname: hostname::get().unwrap().into_string().unwrap(),
    motherboard_id,
    cpu_model,
    smt_disabled,
    turbo_disabled,
    cpu_ids: cores_info.iter().map(|(c, i)| (*c, i.join(" ; "))).collect(),
    git_hash: GIT_HASH.to_owned(),
    ram_swap_id,
    os_release,
    kernel
  }
}
