use log::*;
use std::collections::{HashMap, HashSet};
use std::fmt;
use std::fs::File;
use std::io::Write;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use chrono::prelude::*;

use rowhammer::{*, config::*};
use rowhammer::utils::{*, dram::*, serialize::*};


//Simple rowhammer templating tool using double-sided rowhammer
//Templater config is in templater_config.toml
//This tool can also perform the templating with different hammer slowdowns
//(garbage between hammer accesses) 
//The templater output contains the discoved bitflips per amount of insert 
//garbage code
//To template without interleaved garbage code, set `garbage_count_start` 
//to 0, and `garbage_count_end` to 1 in the config file
//On ctrl-c, the templating stops and the results so far are exported
//USAGE: sudo ./templater <arbitrary_id> [threshold]
//If a threshold is given, the bitflips that flipped in less experiment rounds
//are filtered out of the final results (defaults to 1)

#[derive(PartialEq, Eq, Hash)]
pub struct Setup {
  pub victim_rows: Vec<RowAndInitValue>,
  pub aggr_pattern: AggressorPattern
}

impl Setup {
  pub fn iter_all_frames(&self) -> impl Iterator<Item = u64> + '_ {
    self.victim_rows.iter().chain(
      self.aggr_pattern.into_iter()).map(|x| x.frames.clone()).flatten()
  }
}

impl fmt::Display for Setup {
  fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    write!(f, "victim rows: {}; aggressor rows: {}",
      self.victim_rows.iter().map(|x| {
        format!("0x{:x}", x.frames[0])
      }).collect::<Vec<_>>().join(","),
      self.aggr_pattern.into_iter().map(|x| {
        format!("0x{:x}", x.frames[0])
      }).collect::<Vec<_>>().join(","))
  }
}

pub fn main() {
  info!("Starting templater");
  let start_time = Local::now();

  //init
  assert!(std::env::args().len() >= 2,
    "Provide an experiment ID, and optionally a threshold");
  let mut args = std::env::args().skip(1);
  let id = args.next().unwrap();
  let threshold = args.next().map_or(1, |x| x.parse().unwrap());
  let mut logger_builder = rowhammer::configure();
  //RUST_LOG env var does not work anymore when using `filter_module`
  //logger_builder.filter_module("rowhammer", LevelFilter::Info);
  //logger_builder.filter_module("rowhammer::utils::hammer",LevelFilter::Debug);
  logger_builder.init();

  let templater_config: TemplaterConfig =
    files::parse_toml(TEMPLATER_CONFIG_PATH);
  let dram_config: DRAMConfig = dram::create_config();
  let host_config: HostConfig = host::read_config();
  let hammer_count = templater_config.hammer_count;

  let (frames_to_allocate, setups) =
    parse_hammer_pattern(&templater_config, &dram_config);

  //allocate all required frames
  let frame2map =
    allocation::allocate_pages(frames_to_allocate, templater_config.drop_frac);

  //remove the row setups for which a page frame is missing
  let len_before = setups.len();
  let (setups, discards) = filter_whole_setups(setups, &frame2map);
  let comment = format!("Testing {} row setups, dropped {} row setups: \n{}",
    setups.len(), len_before - setups.len(),
    discards.iter().map(|f| format!("  - {}\n", f)).collect::<String>());
  info!("{}", comment);

  //collection of all unique aggressor patterns: pattern(small vector) -> idx
  let mut discovered_aggr_patterns = HashMap::new();
  //collection of all unique disovered victims:
  //(frame_addr, BitFlip, discovered_aggr_patterns_idx) -> idx
  let mut discovered_victims = HashMap::new();
  //the discovered victims for the full experiment
  //garbage_count -> [experiment_round x [discovered_victims_idx of the victim]]
  let mut distribution: HashMap<u32, Vec<Vec<usize>>> = HashMap::new();

  //ctrl-c handler: stop templating and write the results so far
  let interupted = Arc::new(AtomicBool::new(false));
  let inter = interupted.clone();
  ctrlc::set_handler(move || {
    inter.store(true, Ordering::SeqCst);
    warn!("Exiting prematurely");
  }).expect("Error setting Ctrl-C handler");

  //hammer loop
  for rep in 0..templater_config.repetition {
    if interupted.load(Ordering::SeqCst) {break;}
    for garbage_count in templater_config.garbage_count_start
      ..templater_config.garbage_count_end
    {
      if interupted.load(Ordering::SeqCst) {break;}
      info!("Starting experiment {} with garbage count {}", rep, garbage_count);
      let mut experiment_duration = 0u128;

      for setup in &setups {
        if interupted.load(Ordering::SeqCst) {break;}
        let aggr_pattern = &setup.aggr_pattern;
        let victim_rows = &setup.victim_rows;
        //init victim row and their aggressor row
        mem_init::initialize_rows(victim_rows.iter(), &frame2map);
        mem_init::initialize_rows(aggr_pattern.into_iter(), &frame2map);

        //hammer
        let aggrs = aggr_pattern.aggr_rows_to_virt(&frame2map);
        let duration =
          //hammer::hammer_rust(&aggrs, hammer_count);
          //hammer::hammer_asm(&aggrs, garbage_count, hammer_count);
          hammer::hammer_jit(&aggrs, garbage_count, hammer_count);
        experiment_duration += duration.as_micros();

        //check for bitflips in all victim rows
        let found_victims: Vec<(u64, BitFlip)> =
          victim_rows.iter().map(|victim_row| {
            flip_checks::check_row_for_flips(
              &frame2map, victim_row.frames[0], victim_row.init)
        }).flatten().collect();

        for victim in found_victims {
          //add discovered aggr pattern
          let l = discovered_aggr_patterns.len();
          let aggr_pattern_idx = *discovered_aggr_patterns
            .entry(aggr_pattern).or_insert_with(|| l);

          //add discovered victim
          let l = discovered_victims.len();
          let victim_setup = (victim.0, victim.1, aggr_pattern_idx);
          let idx = discovered_victims.entry(victim_setup).or_insert_with(|| l);

          distribution.entry(garbage_count)
            .or_insert(vec!(Vec::new(); templater_config.repetition))[rep]
            .push(*idx);
        }
      }

      info!("Average hammer time over all row setups in this experiment: {}us",
            experiment_duration / setups.len() as u128);
    }
  }

  info!("Finalizing data structures and emitting results to file");

  let end_time = Local::now();
  let time_format = "%d/%m/%Y %H:%M";
  let timestamp = format!("{} - {}",
    start_time.format(time_format), end_time.format(time_format));

  //finalize list of unique victims and aggressor patterns for exportation
  let mut victims = vec!((0, Default::default(), 0); discovered_victims.len());
  discovered_victims.into_iter().for_each(|(k,v)| victims[v] = k);
  let mut aggr_patterns =
    vec!(Default::default(); discovered_aggr_patterns.len());
  discovered_aggr_patterns.into_iter()
    .for_each(|(k,v)| aggr_patterns[v] = k.clone());

  //print number of bits that flipped in all experiment rounds
  info!("Found {} unique flips", victims.len());
  for (k,v) in &distribution {
    //you know what, let's use another hashmap, i don't care anymore
    //counts the occurances of a specific bitflip accross the different 
    //experiment rounds
    let mut histogram = HashMap::new();
    //iter over all experiment rounds
    //flips within a round are unique, so we can use flatten here
    v.iter().flatten().for_each(|i|
      *histogram.entry(i).or_insert_with(|| 0) += 1);
    //plot your beautiful histogram here

    //filter out the flips that didn't flip in enough experiment rounds
    let t = histogram.iter().fold(0, |mut acc, (_,v)| {
      if *v >= threshold {acc += 1};
      acc
    });
    info!("  garbage_count {}: {} bits flipped in >= {} experiment rounds",
      k, t, threshold);
  }

  //write results to file
  let mut file = File::create(format!("templating{}.json", id)).unwrap();
  write!(file, "{}", serde_json::to_string(&MemoryTemplate {
    templater_config, dram_config, host_config, timestamp,
    comment, victims, aggr_patterns, distribution
  }).unwrap()).unwrap();
}

//create all double sided rowhammer patterns in the given DRAM region
fn parse_hammer_pattern(
  templater_config: &TemplaterConfig, dram_config: &DRAMConfig
) -> (HashSet<u64>, Vec<Setup>) {
  info!("Generating double-sided patterns for rows: {} - {} and banks {:?}",
    templater_config.row_start, templater_config.row_end,
    templater_config.bank_idxs);

  let mut frames_to_allocate = HashSet::new();
  //all combinations of rows and init values
  let mut setups = Vec::new();

  //collect all required (victim and aggressor) frames
  for row_idx in templater_config.row_start
    ..(templater_config.row_end - templater_config.pattern.len() as u64 + 1)
  {
    for bank_idx in &templater_config.bank_idxs {
      for init_value in &templater_config.init_values {
        let mut setup = Setup {
          victim_rows: Vec::new(),
          aggr_pattern: AggressorPattern {pattern: Vec::new()}
        };

        for (i,c) in templater_config.pattern.chars().enumerate() {
          let dram_addr = DRAMAddr {
            bank: *bank_idx, row: row_idx + i as u64, column: 0};
          let phys_addr = dram::dram_to_phys(&dram_addr, &dram_config);
          let frames = utils::get_frames_in_row(phys_addr);
          frames_to_allocate.extend(frames.iter());

          let victim_init = init_value.victim_init;
          let aggr_init = init_value.aggr_init;
          match c {
            'A' => {
              setup.aggr_pattern.pattern.push(RowAndInitValue {
                frames: frames.clone(), init: aggr_init});
            },
            'V' => {
              setup.victim_rows.push(RowAndInitValue {
                frames: frames.clone(), init: victim_init});
            }
            _ => panic!("Unknown rowhammer pattern")
          }
        }
        setups.push(setup);
      }
    }
  }

  (frames_to_allocate, setups)
}

//returns (arg:setups with the incomplete setups removed, all discarded setups)
pub fn filter_whole_setups(
  setups: Vec<Setup>,
  frame2map: &Frame2Map,
) -> (Vec<Setup>, HashSet<Setup>) {
  info!("Removing the patterns with unallocated pages");

  let mut ret = Vec::new();
  let mut discard = HashSet::new();
  setups.into_iter().for_each(|setup| {
    if setup.iter_all_frames().any(|f| !frame2map.contains_key(&f)) {
      discard.insert(setup);
    } else {
      ret.push(setup);
    }
  });

  (ret, discard)
}
