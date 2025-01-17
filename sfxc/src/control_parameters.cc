#include "control_parameters.h"
#include "output_header.h"
#include "utils.h"

#include <fstream>
#include <set>
#include <cstring>
#include <cctype>
#include <math.h>

#include <libgen.h>

#include <json/json.h>
#include <algorithm>


Control_parameters::Control_parameters()
    : initialised(false) {}

Control_parameters::Control_parameters(const char *ctrl_file,
                                       const char *vex_file,
                                       std::ostream& log_writer)
    : initialised(false) {
  if(!initialise(ctrl_file, vex_file, log_writer))
    sfxc_abort();
}

bool
Control_parameters::
initialise(const char *ctrl_file, const char *vex_file,
           std::ostream& log_writer) {
  ctrl_filename = ctrl_file;
  vex_filename = vex_file;

  { // parse the control file
    Json::Reader reader;
    std::ifstream in(ctrl_file);
    if (!in.is_open()) {
      log_writer << "Could not open control file [" << ctrl_file << "]" << std::endl;
      return false;
    }
    bool ok = reader.parse(in, ctrl);
    if ( !ok ) {
      // report to the user the failure and their locations in the document.
      log_writer  << "Failed to parse control file\n"
      << reader.getFormatedErrorMessages()
      << std::endl;
      return false;
    }
  }

  { // VEX file
    std::ifstream in(vex_file);
    if (!in.is_open()) {
      log_writer << "Could not open vex file [" <<vex_file<<"]"<< std::endl;
      return false;
    }

    // parse the vex file
    if (!vex.open(vex_file)) {
      log_writer << "Could not parse vex file ["<<vex_file<<"]" << std::endl;
      return false;
    }
  }

  // set to the default
  if (ctrl["delay_directory"] == Json::Value()) {
    ctrl["delay_directory"] = "file:///tmp/";
  }

  // set the subbands
  if (ctrl["channels"] == Json::Value()) {
    std::set<std::string> result_set;

    // insert all channels
    for (Vex::Node::const_iterator frq_block = vex.get_root_node()["FREQ"]->begin();
         frq_block != vex.get_root_node()["FREQ"]->end(); ++frq_block) {
      for (Vex::Node::const_iterator freq_it = frq_block->begin("chan_def");
           freq_it != frq_block->end("chan_def"); ++freq_it) {
        result_set.insert(freq_it[4]->to_string());
      }
    }
    for (std::set<std::string>::const_iterator set_it = result_set.begin();
         set_it != result_set.end(); ++set_it) {
      ctrl["channels"].append(*set_it);
    }
  }

  // set the scans
  if (ctrl["scans"] == Json::Value()) {
    for (Vex::Node::const_iterator scan = vex.get_root_node()["SCHED"]->begin();
	 scan != vex.get_root_node()["SCHED"]->end(); scan++) {
      ctrl["scans"].append(scan.key());
    } 
  }

  // Checking reference station
  if (ctrl["reference_station"] == Json::Value()) {
    ctrl["reference_station"] = "";
  }

  // Checking message level
  if (ctrl["message_level"] == Json::Value()) 
    ctrl["message_level"] = 1;

  if (ctrl["pulsar_binning"] == Json::Value()){
    ctrl["pulsar_binning"] = false;
  }else if(ctrl["pulsar_binning"].asBool()==true){
    // use pulsar binning
    DEBUG_MSG("Using pulsar binning");

    if (ctrl["pulsars"] == Json::Value()){
      log_writer << "Error : No pulsars block in control file.\n";
      return false;
    }
    Json::Value::iterator it = ctrl["pulsars"].begin();
    if(*it==Json::Value()){
      log_writer <<  "Error : Empty pulsars block in control file.\n";
      return false;
    }
    while(it!=ctrl["pulsars"].end()){
      if((*it)["interval"] == Json::Value()){
        (*it)["interval"].append(0.0); 
        (*it)["interval"].append(1.0);
      }
      if((*it)["nbins"]==Json::Value()){
        // If nbins is not set we default to the maxium possible (PULSAR_PERIOD/DURATION_SINGLE_FFT)
        // signalled by nbins = 0 
        (*it)["nbins"] = 0;
      }
      it++;
    }
  }
  if (ctrl["phased_array"] == Json::Value())
    ctrl["phased_array"] = false;

  if (ctrl["multi_phase_center"] == Json::Value()){
    ctrl["multi_phase_center"] = false;
    if(!ctrl["pulsar_binning"].asBool()){
      Vex::Node::const_iterator it = vex.get_root_node()["SCHED"]->begin();
      while(it != vex.get_root_node()["SCHED"]->end()){
        int n_sources = 0;
        Vex::Node::const_iterator sources = it->begin("source");
        while(sources != it->end("source")){
          n_sources++;
          sources++;
        }

        if(n_sources > 1){
          ctrl["multi_phase_center"] = true;
          break;
        }
        it++;
      }
    }
  } else if ((ctrl["multi_phase_center"].asBool() == true) && 
             (ctrl["pulsar_binning"].asBool() == true)){
    std::cout << "Pulsar binning cannot be used together with multiple phase centers\n";
    return false;
  }
  // No phased array in pulsar binning mode
  if ((ctrl["phased_array"].asBool() == true) &&
      (ctrl["pulsar_binning"].asBool() == true)){
    std::cout << "Pulsar binning cannot be used in phase array mode\n";
    return false;
  }
  // Set default windowing function, if necessary
  if (ctrl["window_function"] == Json::Value()){
    if (ctrl["multi_phase_center"].asBool())
      ctrl["window_function"] = "NONE";
    else
      ctrl["window_function"] = "HANN";
  }
  // Set the fft sizes
  if (ctrl["fft_size_correlation"] == Json::Value()){
    int min_size = ctrl["multi_phase_center"].asBool() ? 4096 : 256;

    if (ctrl["fft_size_delaycor"] != Json::Value())
      min_size = std::max(min_size, ctrl["fft_size_delaycor"].asInt());

    ctrl["fft_size_correlation"] = std::max(min_size, number_channels());
  }
  if (ctrl["fft_size_delaycor"] == Json::Value())
    ctrl["fft_size_delaycor"] = std::min(256, ctrl["fft_size_correlation"].asInt());
  
  // Set the sub integartion time
  if(ctrl["sub_integr_time"] == Json::Value()){
    double integr_time_usec = round(integration_time().get_time_usec());
    if (ctrl["multi_phase_center"].asBool()){
      // Default to +/- 20 ms sub integrations
      ctrl["sub_integr_time"] = std::min(integr_time_usec, 20480.);
    }else{
      // Default to 125 ms sub integrations
      ctrl["sub_integr_time"] = std::min(integr_time_usec, 125000.);
    }
  }

  // Set PhaseCal integration time
  if(ctrl["phasecal_integr_time"] == Json::Value()) {
    if (ctrl["phasecal_file"].asBool())
      ctrl["phasecal_integr_time"] = 10;
    else
      ctrl["phasecal_integr_time"] = 0;
  }

  // By default we abort the correlation if one of the input streams
  // contains no data
  if(ctrl["exit_on_empty_datastream"] == Json::Value())
    ctrl["exit_on_empty_datastream"] = true;

  if (ctrl["start"].asString().compare("now") == 0) {
    char *now;
    time_t t;
    struct tm tm;
    ::time(&t);
    ::gmtime_r(&t, &tm);
    ::asprintf(&now, "%dy%dd%dh%dm%ds", tm.tm_year + 1900, tm.tm_yday + 1,
	       tm.tm_hour, tm.tm_min, tm.tm_sec);
    ctrl["start"] = now;
  }

  if (ctrl["stop"].asString().compare("end") == 0)
    ctrl["stop"] = vex.get_stop_time_of_experiment();

  // Get start date
  start_time = Time(vex.get_start_time_of_experiment());
  initialised = true;

  return true;
}

int
Control_parameters::reference_station_number() const {
  if (ctrl["reference_station"] == Json::Value())
    return -1;
  std::string reference_station = ctrl["reference_station"].asString();
  if (reference_station == "")
    return -1;

  return station_number(ctrl["reference_station"].asString());
}

bool
Control_parameters::check_data_source(std::ostream &writer,
				      const Json::Value& value) const
{
  bool ok = true;

  for (Json::Value::const_iterator source_it = value.begin();
       source_it != value.end(); source_it++) {
    std::string filename = create_path((*source_it).asString());

    if (filename.find("file://")  != 0 &&
	filename.find("mk5://") != 0) {
      ok = false;
      writer << "Ctrl-file: invalid data source '" << filename << "'"
	     << std::endl;
    }
  }

  return ok;
}

bool
Control_parameters::check(std::ostream &writer) const {
  bool ok = true;

  // check start and stop time
  if (ctrl["start"] == Json::Value()) {
    ok = false;
    writer << "Ctrl-file: start time not defined" << std::endl;
  } else {
    if (ctrl["stop"] == Json::Value()) {
      ok = false;
      writer << "Ctrl-file: stop time not defined" << std::endl;
    } else {
      Time start(ctrl["start"].asString());
      Time stop(ctrl["stop"].asString());
      if (stop <= start) {
        ok = false;
        writer << "Ctrl-file: stop time before start time" << std::endl;
      }
    }
  }

  { // Check integration time
    if (ctrl["integr_time"] == Json::Value()){
      ok = false;
      writer << "Ctrl-file: Integration time not set" << std::endl;
    } else {
      Time integr_time(ctrl["integr_time"].asDouble()*1000000);
      if (integr_time < Time(0)) {
        ok = false;
        writer << "Ctrl-file: Integration time is negative" << std::endl;
      }

      // Check sub integration time
      if (ctrl["sub_integr_time"] != Json::Value()){
        Time sub_integr_time(ctrl["sub_integr_time"].asDouble());
        if (sub_integr_time < Time(0)) {
          ok = false;
          writer << "Ctrl-file: Sub integration time is negative" << std::endl;
        } else if (integr_time < sub_integr_time){
          ok = false;
          writer << "Ctrl-file: Sub integration time is larger than the integration time" << std::endl;
        }
      }
    }
  }

  { // Check PhaseCal
    if (ctrl["phasecal_integr_time"].asInt() != 0 &&
	ctrl["phasecal_file"] == Json::Value()) {
      ok = false;
      writer << "Ctrl-file: PhaseCal output file not defined" << std::endl;
    } else {
      Time phasecal_integr_time(ctrl["phasecal_integr_time"].asInt() * 1000000);
      if (phasecal_integr_time < Time(0)) {
	ok = false;
	writer << "Ctrl-file: Phasecal integration time is negative" << std::endl;
      }
    }
  }

  { // Check FFT
    int fft = 0;
    if (ctrl["fft_size_delaycor"] != Json::Value()){
      if(!isPower2(ctrl["fft_size_delaycor"].asInt())){
        ok = false;
        writer << "Ctrl-file: fft_size_delaycor is not a power of two" << std::endl;
      }
      fft += 1;
    }
    if (ctrl["fft_size_correlation"] != Json::Value()){
      if(!isPower2(ctrl["fft_size_correlation"].asInt())){
        ok = false;
        writer << "Ctrl-file: fft_size_correlation is not a power of two" << std::endl;
      }
      if (ctrl["fft_size_correlation"].asInt() < ctrl["number_channels"].asInt()){
        ok = false;
        writer << "Ctrl-file: fft_size_correlation cannot be smaller than the number of channels\n";
      }
      fft += 1;
    }
    if(fft == 2){
      if(ctrl["fft_size_correlation"].asInt() < ctrl["fft_size_delaycor"].asInt()){
        ok = false;
        writer << "Ctrl-file: fft_size_correlation should not be smaller than fft_size_delaycor." << std::endl;
      }
    }
  }

  { // Check stations and reference station
    if (ctrl["stations"] != Json::Value()) {
      std::set<std::string> stations_set;
      for (size_t station_nr = 0;
           station_nr < ctrl["stations"].size(); ++station_nr) {
        std::string station_name = ctrl["stations"][station_nr].asString();
        if (stations_set.find(station_name) != stations_set.end()) {
          ok = false;
          writer << "Ctrl-file: Station " << station_name 
                 << " appears multiple times in the stations list\n";
        } else {
          stations_set.insert(station_name);
        }
        if (ctrl["data_sources"][station_name] == Json::Value()) {
          ok = false;
          writer << "Ctrl-file: No data source defined for "
          << station_name << std::endl;
        } else if (ctrl["data_sources"][station_name].size()==0) {
          ok = false;
          writer << "Ctrl-file: Empty list of data sources for "
          << ctrl["data_sources"][station_name]
          << std::endl;
        } else {
          const Json::Value sources =
            ctrl["data_sources"][station_name];
	  if (sources.isObject()) {
	    for (Json::Value::const_iterator source_it = sources.begin();
		 source_it != sources.end(); source_it++)
	      check_data_source(writer, *source_it);
	  } else {
	    check_data_source(writer, sources);
	  }
	}
      }

      #ifdef USE_MPI
      // Check if there enough mpi nodes for the correlation
      // NB We assume that all scans in the correlation use the same $MODE
      int numproc, minproc;
      MPI_Comm_size(MPI_COMM_WORLD, &numproc);
      std::string mode = get_vex().get_mode(scan(scan(ctrl["start"].asString())));
      minproc = 3 + number_inputs() + number_correlation_cores_per_timeslice(mode);

      if (numproc < minproc) {
        writer << "#correlator nodes < #freq. channels, use at least "
               << minproc << " nodes." << std::endl;
        ok = false;
      }
      #endif
    } else {
      ok = false;
      writer << "Ctrl-file: Stations not found" << std::endl;
    }

    if (ctrl["reference_station"] != Json::Value()) {
      if (ctrl["reference_station"].asString() != "") {
        int idx = -1;
        for (int i = 0; i < number_stations(); i++) {
          if (ctrl["stations"][i].asString() == 
              ctrl["reference_station"].asString()) {
            idx = i;
            break;
          }
        }
        if (idx == -1) {
          ok = false;
          writer 
            << "Ctrl-file: Reference station not one of the input stations"
            << std::endl;
        }
      }
    } else {
      ok = false;
      writer << "Ctrl-file: Reference station not found" << std::endl;
    }
  }

  { // Check output file
    if (ctrl["output_file"] != Json::Value()) {
      std::string output_file = create_path(ctrl["output_file"].asString());
      if (strncmp(output_file.c_str(), "file://", 7) != 0) {
        ok = false;
        writer
        << "Ctrl-file: Correlation output should start with 'file://'"
        << std::endl;
      }
    } else {
      ok = false;
      writer << "ctrl-file: output file not defined" << std::endl;
    }
  }

  // Check phasecal file
  if (ctrl["phasecal_file"] != Json::Value()) {
    std::string filename = create_path(ctrl["phasecal_file"].asString());
    if (strncmp(filename.c_str(), "file://", 7) != 0) {
      ok = false;
      writer << "Ctrl-file: Phasecal output should start with 'file://'"
	     << std::endl;
    }
  }

  // Check mask parameters
  if (ctrl["mask"] != Json::Value()) {
    if (ctrl["mask"]["mask"] != Json::Value()) {
      std::string filename = create_path(ctrl["mask"]["mask"].asString());
      if (strncmp(filename.c_str(), "file://", 7) != 0) {
        ok = false;
        writer << "Ctrl-file: Mask file should start with 'file://'"
	       << std::endl;
      }
    }
    if (ctrl["mask"]["window"] != Json::Value()) {
      std::string filename = create_path(ctrl["mask"]["window"].asString());
      if (strncmp(filename.c_str(), "file://", 7) != 0) {
        ok = false;
        writer << "Ctrl-file: Window file should start with 'file://'"
	       << std::endl;
      }
    }
  }

  // Check window function
  if (ctrl["window_function"] != Json::Value()){
    std::string window = ctrl["window_function"].asString();
    for(int i = 0; i < window.size(); i++)
      window[i] = toupper(window[i]);
    if ((window != "RECTANGULAR") and (window != "COSINE") and (window != "HAMMING") and 
      (window != "HANN") and (window != "PFB") and (window != "NONE")){
      writer << "Invalid window function " << window 
             << ", valid choises are : RECTANGULAR, COSINE, HAMMING, HANN, PFB, and NONE" << std::endl;
      ok = false;
    }
  }
  
  // Check pulsar binning
  if (ctrl["pulsar_binning"].asBool()){
    // use pulsar binning
    if (ctrl["pulsars"] == Json::Value()){
      ok=false;
      writer << "ctrl-file : No pulsars block in control file.\n";
    }else{
      Json::Value::const_iterator it = ctrl["pulsars"].begin();
      if(*it==Json::Value()){
        ok = false;
        writer << "ctrl-file : Empty pulsars block in control file.\n";
      }else{
        while(it!=ctrl["pulsars"].end()){
          if((*it)["interval"].size() != 2){
            ok = false;
            writer << "ctrl-file : Invalid number of arguments in interval field.\n";
          }else{
            Json::Value interval = (*it)["interval"];
            unsigned int zero=0,one=1; // needed to prevent compilation error
            if ((interval[zero].asDouble()<0.)||(interval[zero].asDouble()>1)||
                (interval[one].asDouble()<0.)||(interval[one].asDouble()>=2)||
                (interval[one].asDouble() - interval[zero].asDouble() <= 0) ||
                (interval[one].asDouble() - interval[zero].asDouble() > 1)){
              ok = false;
              writer << "ctrl-file : Invalid range in interval field.\n";
            }
          }
          if((*it)["nbins"].asInt() < 0){
            ok= false;
            writer << "ctrl-file : Invalid number of bins : " << (*it)["nbins"].asInt()<<".\n";
          }
          if((*it)["polyco_file"] == Json::Value()){
            ok = false;
            writer << "ctrl-file : No polyco files specified.\n";
          }else if((*it)["polyco_file"].size() > 1 ){
            ok = false;
            writer << "ctrl-file : More than one polyco file specified for a pulsar.\n";
          } else {
            std::string filename = create_path((*it)["polyco_file"].asString());
            if (filename.find("file://") != 0){
              ok = false;
              writer << "Ctrl-file: polyco file definition doesn't start with file://  '" << filename << "'\n";
            }else{
              // Check whether the file exists
              std::ifstream in(create_path(filename).c_str()+7);
              if (!in.is_open()) {
               ok = false;
               writer << "Ctrl-file: Could not open polyco file : " << filename << std::endl;
              }else{
                writer << "Parsing polyco file : " << filename << "\n";
                Pulsar_parameters pc(writer);
                std::vector<Pulsar_parameters::Polyco_params> param;
                if (!pc.parse_polyco(param, filename.substr(7))){
                  ok = false;
                  writer << "Ctrl-file: Error parsing polyco file : " << filename << std::endl;
                }
              }
            }
          }
          it++;
        }
      }
    }
  }
  return ok;
}

Time
Control_parameters::get_start_time() const {
  return Time(ctrl["start"].asString());
}

Time
Control_parameters::get_stop_time() const {
  return Time(ctrl["stop"].asString());
}
void 
Control_parameters::set_reader_offset(const std::string &station, const Time t){
  reader_offsets[station] = t;
}

std::vector<std::string>
Control_parameters::data_sources(const std::string &station) const {
  std::vector<std::string> result;
  const Json::Value sources = ctrl["data_sources"][station];
  SFXC_ASSERT(sources != Json::Value());
  if (sources.isArray()) {
    for (size_t i = 0; i < sources.size(); i++)
      result.push_back(create_path(sources[i].asString()));
  }
  return result;
}

std::vector<std::string>
Control_parameters::data_sources(const std::string &station,
				 const std::string &datastream) const {
  std::vector<std::string> result;
  Json::Value sources = ctrl["data_sources"][station];
  SFXC_ASSERT(sources != Json::Value());
  if (sources.isObject()) {
    sources = sources[datastream];
    for (size_t i = 0;  i < sources.size(); i++)
      result.push_back(create_path(sources[i].asString()));
    return result;
  }
  return data_sources(station);
}

std::string
Control_parameters::get_output_file() const {
  return create_path(ctrl["output_file"].asString());
}

std::string
Control_parameters::get_phasecal_file() const {
  return create_path(ctrl["phasecal_file"].asString());
}

std::string
Control_parameters::get_tsys_file() const {
  return create_path(ctrl["tsys_file"].asString());
}

std::string
Control_parameters::station(int i) const {
  return ctrl["stations"][i].asString();
}

size_t
Control_parameters::number_stations() const {
  return ctrl["stations"].size();
}

size_t
Control_parameters::number_inputs() const {
  size_t count = 0;
  for (int i = 0; i < number_stations(); i++) {
    Json::Value sources = ctrl["data_sources"][station(i)];
    if (sources.isObject())
      count += sources.size();
    else
      count++;
  }
  //  SFXC_ASSERT(count == number_stations());
  return count;
}

std::string
Control_parameters::scan(int i) const {
  return ctrl["scans"][i].asString();
}

size_t
Control_parameters::number_scans() const {
  return ctrl["scans"].size();
}

Time
Control_parameters::integration_time() const {
  return Time(round(ctrl["integr_time"].asDouble()*1000000));
}

Time
Control_parameters::sub_integration_time() const {
    return Time(ctrl["sub_integr_time"].asDouble());
}

Time
Control_parameters::phasecal_integration_time() const {
  return Time(ctrl["phasecal_integr_time"].asInt() * 1000000);
}

int
Control_parameters::slices_per_integration() const {
  if (ctrl["slices_per_integration"] == Json::Value())
    return 1;
  
  return ctrl["slices_per_integration"].asInt();
}

bool
Control_parameters::exit_on_empty_datastream() const{
  return ctrl["exit_on_empty_datastream"].asBool();
}

int
Control_parameters::number_channels() const {
  return ctrl["number_channels"].asInt();
}

int
Control_parameters::fft_size_delaycor() const {
  return ctrl["fft_size_delaycor"].asInt();
}

int
Control_parameters::fft_size_correlation() const {
  return ctrl["fft_size_correlation"].asInt();
}

double
Control_parameters::LO_offset(const std::string &station, int integration_nr) const {
  if (ctrl["LO_offset"] == Json::Value())
    return 0;
  if (ctrl["LO_offset"][station] == Json::Value())
    return 0;
  if (ctrl["LO_offset"][station].isArray()) {
    int i = 0; // need variable to prevent operator overload ambiguity
    double start = ctrl["LO_offset"][station][i++].asDouble();
    double end = ctrl["LO_offset"][station][i++].asDouble();
    int nstep = ctrl["LO_offset"][station][i].asInt();
    return start + (integration_nr % nstep) * (end - start) / nstep;
  }
  return ctrl["LO_offset"][station].asDouble();
}

double
Control_parameters::extra_delay(const std::string &channel,
				const std::string &station,
				const std::string &mode) const {
  if (ctrl["extra_delay"] == Json::Value())
    return 0;
  if (ctrl["extra_delay"][station] == Json::Value())
    return 0;

  if (ctrl["extra_delay"][station][channel] != Json::Value())
    return ctrl["extra_delay"][station][channel].asDouble();

  std::string pol(1, polarisation(channel, station, mode));

  if (ctrl["extra_delay"][station][pol] != Json::Value())
    return ctrl["extra_delay"][station][pol].asDouble();

  return 0;
}

int
Control_parameters::extra_delay_in_samples(const std::string &channel,
					   const std::string &station,
					   const std::string &mode) const {
  double delay = extra_delay(channel, station, mode);
  return (int) floor(delay * sample_rate(mode, station) + 0.5);
}

int
Control_parameters::tsys_freq(const std::string &station) const {
  if (ctrl["tsys_freq"] == Json::Value())
    return 80;
  if (ctrl["tsys_freq"][station] == Json::Value())
    return 80;

  return ctrl["tsys_freq"][station].asInt();
}

int
Control_parameters::window_function() const{
  int windowval = SFXC_WINDOW_NONE;
  if (ctrl["window_function"] != Json::Value()){
    std::string window = ctrl["window_function"].asString();
    for(int i = 0; i < window.size(); i++)
      window[i] = toupper(window[i]);
    if(window == "RECTANGULAR")
      windowval = SFXC_WINDOW_RECT;
    else if(window == "COSINE")
      windowval = SFXC_WINDOW_COS;
    else if(window == "HAMMING")
      windowval = SFXC_WINDOW_HAMMING;
    else if(window == "HANN")
      windowval = SFXC_WINDOW_HANN;
    else if (window == "NONE")
      windowval = SFXC_WINDOW_NONE;
    else if (window == "PFB")
      windowval = SFXC_WINDOW_PFB;
  }
  return windowval;
}

int
Control_parameters::job_nr() const {
  if (ctrl["job"] == Json::Value())
    return 0;
  else
    return ctrl["job"].asInt();
}

int
Control_parameters::subjob_nr() const {
  if (ctrl["subjob"] == Json::Value())
    return 0;
  else
    return ctrl["subjob"].asInt();
}

std::string
Control_parameters::sideband(int i) const {
  return ctrl["subbands"][i]["sideband"].asString();
}

std::string
Control_parameters::reference_station() const {
  return ctrl["reference_station"].asString();
}

std::string
Control_parameters::setup_station() const {
  if (ctrl["setup_station"] == Json::Value())
    return station(0);
  else
    return ctrl["setup_station"].asString();
}

std::string
Control_parameters::channel(int i) const {
  return ctrl["channels"][i].asString();
}

int Control_parameters::message_level() const {
  return ctrl["message_level"].asInt();
}

bool Control_parameters::phased_array() const{
  return ctrl["phased_array"].asBool();
}

bool Control_parameters::pulsar_binning() const{
  return ctrl["pulsar_binning"].asBool();
}

bool Control_parameters::multi_phase_center() const{
  return ctrl["multi_phase_center"].asBool();
}

bool
Control_parameters::get_pulsar_parameters(Pulsar_parameters &pars) const{
  if(!pulsar_binning())
    return false;
  for(Json::Value::const_iterator it = ctrl["pulsars"].begin();
      it!=ctrl["pulsars"].end(); it++){
    std::string name= it.key().asString();
    Pulsar_parameters::Pulsar newPulsar;
    if(name.size() > 10)
      name.resize(10);
    strcpy(&newPulsar.name[0], name.c_str());
    newPulsar.nbins = (*it)["nbins"].asInt();
    unsigned int zero=0, one=1; //needed to prevent compiler error
    newPulsar.interval.start = (*it)["interval"][zero].asDouble();
    newPulsar.interval.stop  = (*it)["interval"][one].asDouble();
    if(!pars.parse_polyco(newPulsar.polyco_params,(*it)["polyco_file"].asString().substr(7)))
      return false;
    pars.pulsars.insert(std::pair<std::string,Pulsar_parameters::Pulsar>(name,newPulsar));
  }
  return true;
}

bool
Control_parameters::get_mask_parameters(Mask_parameters &pars) const {
  if (ctrl["mask"] == Json::Value())
    return false;

  pars.normalize = ctrl["mask"]["normalize"].asBool();
  if (ctrl["mask"]["mask"] != Json::Value()) {
    std::string filename = create_path(ctrl["mask"]["mask"].asString());
    std::ifstream infile(filename.c_str() + 7);
    if (!infile) {
      std::cerr << "Could not open mask file " << filename << std::endl;
      sfxc_abort();
    }
    double d;
    while (infile >> d)
      pars.mask.push_back(d);
  }
  if (ctrl["mask"]["window"] != Json::Value()) {
    std::string filename = create_path(ctrl["mask"]["window"].asString());
    std::ifstream infile(filename.c_str() + 7);
    if (!infile) {
      std::cerr << "Could not open window file " << filename << std::endl;
      sfxc_abort();
    }
    double d;
    while (infile >> d)
      pars.window.push_back(d);
  }

  return true;
}

int
Control_parameters::bits_per_sample(const std::string &mode,
                                    const std::string &station) const
{
  if (data_format(station, mode) == "VDIF") {
    const std::string datastreams_name = get_vex().get_section("DATASTREAMS", mode, station);
    if (get_vex().get_version() > 1.5 && datastreams_name == std::string()) {
      std::cerr << "Cannot find $DATASTREAMS reference for " << station
		<< " in mode" << mode << std::endl;
      sfxc_abort();
    }
    if (datastreams_name != std::string()) {
      Vex::Node::const_iterator datastreams = vex.get_root_node()["DATASTREAMS"][datastreams_name];
      for (Vex::Node::const_iterator thread_it = datastreams->begin("thread");
	   thread_it != datastreams->end("thread"); thread_it++) {
	return thread_it[5]->to_int();
      }
    }

    const std::string threads_name = get_vex().get_section("THREADS", mode, station);
    Vex::Node::const_iterator thread = vex.get_root_node()["THREADS"][threads_name];
    for (Vex::Node::const_iterator thread_it = thread->begin("thread");
	 thread_it != thread->end("thread"); thread_it++) {
      return thread_it[5]->to_int();
    }
  }

  if (data_format(station, mode) == "Mark5B") {
    const std::string bitstreams_name = get_vex().get_section("BITSTREAMS", mode, station);
    if (get_vex().get_version() > 1.5 && bitstreams_name == std::string()) {
      std::cerr << "Cannot find $BITSTREAMS reference for " << station
		<< " in mode" << mode << std::endl;
      sfxc_abort();
    }
    if (bitstreams_name != std::string()) {
      Vex::Node::const_iterator bitstream = vex.get_root_node()["BITSTREAMS"][bitstreams_name];
      for (Vex::Node::const_iterator fanout_def_it = bitstream->begin("stream_def");
	   fanout_def_it != bitstream->end("stream_def"); ++fanout_def_it) {
	if (fanout_def_it[1]->to_string() == "mag") {
	  return 2;
	}
      }

      return 1;
    }
  }

  // Fall back on the $TRACKS block for Mark5B recordings if there is
  // no $BITSTREAMS block.
  if (data_format(station, mode) == "Mark4" || data_format(station, mode) == "VLBA" ||
      data_format(station, mode) == "Mark5B") {
    const std::string &track_name = get_vex().get_track(mode, station);
    Vex::Node::const_iterator track = vex.get_root_node()["TRACKS"][track_name];
    for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
         fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
      if (fanout_def_it[2]->to_string() == "mag") {
        return 2;
      }
    }

    return 1;
  }

  sfxc_abort("Unable to determine bits/sample");
}

uint64_t
Control_parameters::sample_rate(const std::string &mode,
				const std::string &station) const
{
  if (get_vex().get_version() > 1.5) {
    // Use the sample rate specified in the $TRACKS, $BITSTREAMS or
    // $DATASTREAMS block if specified.  This is mandatory for VEX 2.0.
    if (data_format(station, mode) == "VDIF") {
      const std::string datastreams_name = get_vex().get_section("DATASTREAMS", mode, station);
      if (datastreams_name == std::string()) {
        std::cerr << "Cannot find $DATASTREAMS reference for " << station
                  << " in mode" << mode << std::endl;
        sfxc_abort();
      }
      Vex::Node::const_iterator datastreams = vex.get_root_node()["DATASTREAMS"][datastreams_name];
      for (Vex::Node::const_iterator thread_it = datastreams->begin("thread");
           thread_it != datastreams->end("thread"); thread_it++) {
        return (int)(thread_it[4]->to_double_amount("Ms/sec") * 1e6);
      }
    }

    if (data_format(station, mode) == "Mark5B") {
      const std::string bitstreams_name = get_vex().get_section("BITSTREAMS", mode, station);
      if (bitstreams_name == std::string()) {
        std::cerr << "Cannot find $BITSTREAMS reference for " << station
                  << " in mode" << mode << std::endl;
        sfxc_abort();
      }
      Vex::Node::const_iterator bitstreams = vex.get_root_node()["BITSTREAMS"][bitstreams_name];
      if (bitstreams->begin("stream_sample_rate") != bitstreams->end())
        return (int)(bitstreams["stream_sample_rate"]->to_double_amount("Ms/sec") * 1e6);
    }

    if ((data_format(station, mode) == "Mark4") || (data_format(station, mode) == "VLBA")) {
      const std::string tracks_name = get_vex().get_section("TRACKS", mode, station);
      if (tracks_name == std::string()) {
        std::cerr << "Cannot find $TRACKS reference for " << station
                  << " in mode" << mode << std::endl;
        sfxc_abort();
      }
      Vex::Node::const_iterator tracks = vex.get_root_node()["TRACKS"][tracks_name];
      if (tracks->begin("sample_rate") != tracks->end())
        return (int)(tracks["sample_rate"]->to_double_amount("Ms/sec") * 1e6);
    }
  }
  const std::string &freq_name = get_vex().get_frequency(mode, station);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];

  return (uint64_t)(freq["sample_rate"]->to_double_amount("Ms/sec") * 1e6);
}

uint64_t
Control_parameters::bandwidth(const std::string &mode,
			      const std::string &station,
			      const std::string &channel) const
{
  const std::string &freq_name = get_vex().get_frequency(mode, station);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];

  for (Vex::Node::const_iterator chan = freq->begin("chan_def"); chan != freq->end("chan_def"); chan++) {
    if (chan[4]->to_string() == channel)
      return (uint64_t)chan[3]->to_double_amount("Hz");
  }

  SFXC_ASSERT(false);
}

int64_t
Control_parameters::channel_freq(const std::string &mode,
				 const std::string &station,
				 const std::string &channel) const
{
  const std::string &freq_name = get_vex().get_frequency(mode, station);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];

  for (Vex::Node::const_iterator chan = freq->begin("chan_def"); chan != freq->end("chan_def"); chan++) {
    if (chan[4]->to_string() == channel)
      return (int64_t)round(chan[1]->to_double_amount("Hz"));
  }

  SFXC_ASSERT(false);
}

std::string
Control_parameters::datastream(const std::string &mode,
			       const std::string &station,
			       const std::string &channel) const
{
  const std::string datastreams_name = get_vex().get_section("DATASTREAMS", mode, station);
  if (get_vex().get_version() > 1.5 && datastreams_name != std::string()) {
    Vex::Node::const_iterator datastreams = vex.get_root_node()["DATASTREAMS"][datastreams_name];
    for (Vex::Node::const_iterator chan = datastreams->begin("channel");
	 chan != datastreams->end("channel"); chan++) {
      if (chan[2]->to_string() == channel)
	return chan[0]->to_string();
    }
  }

  return std::string();
}

std::vector<std::string>
Control_parameters::datastreams(const std::string &station) const
{
  std::vector<std::string> result;
  const Json::Value sources = ctrl["data_sources"][station];
  if (sources.isObject()) {
    for (Json::Value::const_iterator source = sources.begin();
	 source != sources.end(); source++)
      result.push_back(source.key().asString());
  } else {
    result.push_back(std::string());
  }
    
  return result;
}

std::string
Control_parameters::scan_source(const std::string &scan) const {
  return vex.get_root_node()["SCHED"][scan]["source"]->to_string();
}

int Control_parameters::scan(const Time &time) const {
  Vex::Date date(time.date_string());

  int scannr = 0;
  while (scannr < number_scans()) {
    if (date < vex.stop_of_scan(scan(scannr)))
      return scannr;
    scannr++;
  }
  return -1;
}

bool
Control_parameters::
station_in_scan(const std::string &scan, const std::string &station) const {
  for( Vex::Node::const_iterator it = vex.get_root_node()["SCHED"][scan]->begin("station");
       it != vex.get_root_node()["SCHED"][scan]->end("station"); it++){
    if(it[0]->to_string() == station)
      return true;
  }
  return false;
}

Time
Control_parameters::
stop_time(const std::string &scan_name, const std::string &station) const {
  Vex::Node::const_iterator scan =
    vex.get_root_node()["SCHED"][scan_name];
  Time start_time = vex.start_of_scan(scan_name).to_string();
  for (Vex::Node::const_iterator it = scan->begin("station");
       it != scan->end("station"); it++) {
    if (it[0]->to_string() == station)
      return start_time + it[2]->to_double_amount("usec");
  }
  return start_time;
}

size_t
Control_parameters::number_stations_in_scan(const std::string& scan) const {
  size_t n_stations=0;
  for (Vex::Node::const_iterator it =
         vex.get_root_node()["SCHED"][scan]->begin("station");
       it != vex.get_root_node()["SCHED"][scan]->end("station");
       ++it) {
    n_stations++;
  }
  return n_stations;
}

int
Control_parameters::
number_correlation_cores_per_timeslice(const std::string &mode) const {
  if (cross_polarize()) {
    int n_cores=0;
    for (int i=0; i<(int)number_frequency_channels(); i++) {
      int cross = cross_channel(channel(i), mode);
      if ((cross == -1) || (cross > i)) {
        n_cores ++;
      }
    }
    return n_cores;
  } else {
    return number_frequency_channels();
  }
}

size_t
Control_parameters::number_frequency_channels() const {
  return ctrl["channels"].size();
}

// Lookup the name of the channel corresponding to CHANNEL_NR for
// station STATION_NAME in mode MODE_NAME.  Return an empty string if
// no matching channel was founnd.

std::string
Control_parameters::frequency_channel(size_t channel_nr, const std::string& mode_name, const std::string &station_name) const {
  SFXC_ASSERT(channel_nr < number_frequency_channels());

  char pol = polarisation(channel(channel_nr), setup_station(), mode_name);
  if (pol == ' ')
    return std::string(); // Channel not present
  int64_t freq_min, freq_max;
  if (sideband(channel(channel_nr), setup_station(), mode_name) == 'L') {
    freq_max = channel_freq(mode_name, setup_station(), channel(channel_nr));
    freq_min = freq_max - bandwidth(mode_name, setup_station(), channel(channel_nr));
  } else {
    freq_min = channel_freq(mode_name, setup_station(), channel(channel_nr));
    freq_max = freq_min + bandwidth(mode_name, setup_station(), channel(channel_nr));
  }

  const std::string &freq_name = get_vex().get_frequency(mode_name, station_name);
  if (freq_name == std::string()) {
    std::cerr << "Cannot find $FREQ reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }

  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];
  int64_t ch_freq_min, ch_freq_max;
  for (Vex::Node::const_iterator chan = freq->begin("chan_def"); chan != freq->end("chan_def"); chan++) {
    if (chan[2]->to_char() == 'L') {
      ch_freq_max = (int64_t)round(chan[1]->to_double_amount("Hz"));
      ch_freq_min = ch_freq_max - (int64_t)chan[3]->to_double_amount("Hz");
    } else {
      ch_freq_min = (int64_t)round(chan[1]->to_double_amount("Hz"));
      ch_freq_max = ch_freq_min + (int64_t)chan[3]->to_double_amount("Hz");
    }

    // We have a match if the channel corresponding to CHANNEL_NR is
    // wholly conatined in this channel.  This covers the "normal"
    // case where all stations use the same setup as well as the case
    // of mixed 16/32 MHz and 16/64 MHz observations,
    if ((freq_min >= ch_freq_min && freq_max <= ch_freq_max) &&
	pol == polarisation(chan[4]->to_string(), station_name, mode_name))
      return chan[4]->to_string();

    // We also match if this channel is wholly contained in the
    // channel corresponding to CHANNEL_NR.  This covers the case of
    // mixed bandwidth observations where we correlate wide bands but
    // want to include narrower bands in the result.
    if ((ch_freq_min >= freq_min && ch_freq_max <= freq_max) &&
	pol == polarisation(chan[4]->to_string(), station_name, mode_name))
      return chan[4]->to_string();
  }

  return std::string();
}

int
Control_parameters::frequency_number(size_t channel_nr, const std::string& mode_name) const {
  std::set<int64_t> freq_set;
  std::set<int64_t>::const_iterator freq_set_it;
  int64_t frequency = -1;

  const std::string& channel_name = channel(channel_nr);
  const std::string& station_name = setup_station();
  const std::string& freq_name = get_vex().get_frequency(mode_name, station_name);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];
  for (Vex::Node::const_iterator ch_it = freq->begin("chan_def");
       ch_it != freq->end("chan_def");
       ++ch_it) {
    if (ch_it[4]->to_string() == channel_name)
      frequency = (int64_t)round(ch_it[1]->to_double_amount("Hz"));
    freq_set.insert((int64_t)round(ch_it[1]->to_double_amount("Hz")));
  }

  int count = 0;
  for (freq_set_it = freq_set.begin(); freq_set_it != freq_set.end(); ++freq_set_it) {
    if (*freq_set_it == frequency)
      return count;
    count++;
  }

  return -1;
}

const Vex &
Control_parameters::get_vex() const {
  SFXC_ASSERT(initialised);
  return vex;
}

std::string
Control_parameters::get_exper_name() const {
  const Vex::Node &root = get_vex().get_root_node();
  if (root["GLOBAL"]["EXPER"] == root["GLOBAL"]->end()) {
    std::cerr << "Cannot find EXPER in $GLOBAL block" << std::endl;
    sfxc_abort();
  }
  const std::string exper = root["GLOBAL"]["EXPER"]->to_string();
  if (root["EXPER"][exper] == root["EXPER"]->end()) {
    std::cerr << "Cannot find " << exper << " in $EXPER block" << std::endl;
    sfxc_abort();
  }
  if (root["EXPER"][exper]["exper_name"] != root["EXPER"][exper]->end())
    return root["EXPER"][exper]["exper_name"]->to_string();
  return std::string();
}

std::vector<int>
Control_parameters::
get_track_bit_position(const std::string &mode, const std::string &station) const {
  std::vector<int> tracks; // bit positions of all tracks in the vex file
  tracks.resize(64); // tracks from headstack 2 are in position 32-63
  memset(&tracks[0], 0, tracks.size()*sizeof(int));
  const std::string &track_name = get_vex().get_track(mode, station);
  Vex::Node::const_iterator track = vex.get_root_node()["TRACKS"][track_name];
  for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
         fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
    Vex::Node::const_iterator it = fanout_def_it->begin();
    ++it;
    ++it;
    ++it;
    int headstack = it->to_int();
    ++it;
    for (; it != fanout_def_it->end(); ++it) 
      tracks[32 * (headstack-1) + it->to_int() - 2] = 1;
  }
  int bit = -1; // the current bit
  for(int i = 0; i < tracks.size(); i++){
    bit +=tracks[i];
    tracks[i] *= bit;
  }
  return tracks;
}

int
Control_parameters::
n_mark5a_tracks(const std::string &mode, const std::string &station) const {
  const std::string &track_name = get_vex().get_track(mode, station);
  int n_tracks = 0;
  Vex::Node::const_iterator track = vex.get_root_node()["TRACKS"][track_name];
  for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
         fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
    Vex::Node::const_iterator it = fanout_def_it->begin();
    ++it;
    ++it;
    ++it;
    ++it;
    for (; it != fanout_def_it->end(); ++it)
      n_tracks++;
  }
  return n_tracks;
}

void
Control_parameters::
get_mark5a_tracks(const std::string &mode,
                  const std::string &station,
                  Input_node_parameters &input_parameters) const {
  // Bit positions for all tracks in the vex file
  std::vector<int> track_pos = get_track_bit_position(mode, station);
  input_parameters.n_tracks = n_mark5a_tracks(mode, station);

  const std::string &track_name = get_vex().get_track(mode, station);
  Vex::Node::const_iterator track = vex.get_root_node()["TRACKS"][track_name];

  // Determine if data modulation is active
  Vex::Node::const_iterator mod_it = track->begin("data_modulation");
  if (mod_it != track->end() && mod_it->to_string() == "on")
    input_parameters.data_modulation=1;
  else
    input_parameters.data_modulation=0;

  std::vector<int> sign_tracks, mag_tracks;
  for (size_t ch_nr=0; ch_nr < number_frequency_channels(); ch_nr++) {
    const std::string &channel_name = frequency_channel(ch_nr, mode, station);

    if (channel_name != std::string()) {
      // tracks
      Input_node_parameters::Channel_parameters channel_param;
      channel_param.bits_per_sample = 1;
      channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
      channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
      channel_param.frequency_number = frequency_number(ch_nr, mode);
      channel_param.extra_delay_in_samples = extra_delay_in_samples(channel_name, station, mode);
      sign_tracks.resize(0);
      mag_tracks.resize(0);

      for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
           fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
        if (channel_name == fanout_def_it[1]->to_string()) {
          Vex::Node::const_iterator it = fanout_def_it->begin();
          ++it;
          ++it;
          ++it;
          int headstack = it->to_int();
          ++it;
          if(fanout_def_it[2]->to_string() == "sign"){
            for (; it != fanout_def_it->end(); ++it) {
              int track = (headstack - 1)*32 + it->to_int() - 2;
              sign_tracks.push_back(track_pos[track]);
            }
          } else{
            channel_param.bits_per_sample = 2;
            for (; it != fanout_def_it->end(); ++it) {
              int track = (headstack - 1)*32 + it->to_int() - 2;
              mag_tracks.push_back(track_pos[track]);
            }
          }
        }
      }
      if((channel_param.bits_per_sample == 2) && (mag_tracks.size() != sign_tracks.size()))
        sfxc_abort("Number of magnitude tracks do not match the number of sign tracks");
      for(int i=0; i<sign_tracks.size();i++){
        channel_param.tracks.push_back(sign_tracks[i]);
        if(channel_param.bits_per_sample == 2)
          channel_param.tracks.push_back(mag_tracks[i]);
      }
      input_parameters.channels.push_back(channel_param);
    }
  }
}

int
Control_parameters::
n_mark5b_bitstreams(const std::string &mode,
                    const std::string &station) const {
  // First determine if there is a bitstreams section for the current station in the vex file.
  const std::string bitstreams_name = get_vex().get_bitstreams(mode, station);
  Vex::Node::const_iterator bitstream = vex.get_root_node()["BITSTREAMS"][bitstreams_name];
  int n_bitstream = 0;
  // Iterate over the bitstreams
  for (Vex::Node::const_iterator bitstream_it = bitstream->begin("stream_def");
      bitstream_it != bitstream->end("stream_def"); ++bitstream_it) {
    n_bitstream++;
  }
  return n_bitstream;
}

void
Control_parameters::
get_mark5b_tracks(const std::string &mode,
                  const std::string &station,
                  Input_node_parameters &input_parameters) const {
  // First determine if there is a bitstreams section for the current station in the vex file.
  const std::string bitstreams_name = get_vex().get_bitstreams(mode, station);
  if (get_vex().get_version() > 1.5 && bitstreams_name == std::string()) {
    std::cerr << "Cannot find $BITSTREAMS reference for " << station
	      << " in mode" << mode << std::endl;
    sfxc_abort();
  }
  if (bitstreams_name != std::string()) {
    input_parameters.n_tracks = n_mark5b_bitstreams(mode, station);
    // Parse the bitstream section
    Vex::Node::const_iterator bitstream = vex.get_root_node()["BITSTREAMS"][bitstreams_name];
    for (size_t ch_nr=0; ch_nr < number_frequency_channels(); ch_nr++) {
      const std::string &channel_name = frequency_channel(ch_nr, mode, station);

      if (channel_name != std::string()) {
        // Iterate over the bitstreams
        int n_bitstream = 0;
        Input_node_parameters::Channel_parameters channel_param;
        channel_param.bits_per_sample = 1;
	channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
	channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
        channel_param.frequency_number = frequency_number(ch_nr, mode);
        channel_param.extra_delay_in_samples = extra_delay_in_samples(channel_name, station, mode);
        int sign_track, mag_track;
        for (Vex::Node::const_iterator bitstream_it = bitstream->begin("stream_def");
            bitstream_it != bitstream->end("stream_def"); ++bitstream_it) {
          if (channel_name == bitstream_it[0]->to_string()) {
            Vex::Node::const_iterator it = bitstream_it->begin();
            ++it;
            ++it;
            ++it;
            if (bitstream_it[1]->to_string() == "sign"){
              sign_track = it->to_int();
            }else{
              channel_param.bits_per_sample = 2;
              mag_track = it->to_int();
            }
          }
          n_bitstream++;
        }
        // If there are 64 bitstreams then an input word is 8 bytes long, otherewise is is 4 bytes
        int word_size = (n_bitstream <= 32) ? 32 : 64;
        for(int i = 0; i < word_size / n_bitstream;  i++){
          int sign = sign_track + i * n_bitstream;
          channel_param.tracks.push_back(sign);
          if(channel_param.bits_per_sample == 2){
            int magn = mag_track + i * n_bitstream;
            channel_param.tracks.push_back(magn);
          }
        }
        input_parameters.channels.push_back(channel_param);
      }
    }
    return;
  }

  const std::string tracks_name = get_vex().get_track(mode, station);
  if (tracks_name != std::string()) {
    Vex::Node::const_iterator track = vex.get_root_node()["TRACKS"][tracks_name];
    if (track["track_frame_format"]->to_string() == "Mark5B" ||
	track["track_frame_format"]->to_string() == "MARK5B") {
      input_parameters.n_tracks = n_mark5a_tracks(mode, station);
      // Parse the $TRACKS section
      for (size_t ch_nr=0; ch_nr < number_frequency_channels(); ch_nr++) {
	const std::string &channel_name = frequency_channel(ch_nr, mode, station);

	if (channel_name != std::string()) {
	  // Iterate over the tracks, interpreting them as bitstreams
	  int n_bitstream = 0;
	  Input_node_parameters::Channel_parameters channel_param;
	  channel_param.bits_per_sample = 1;
	  channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
	  channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
	  channel_param.frequency_number = frequency_number(ch_nr, mode);
	  channel_param.extra_delay_in_samples = extra_delay_in_samples(channel_name, station, mode);
	  int sign_track, mag_track;
	  for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
	       fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
	    if (channel_name == fanout_def_it[1]->to_string()) {
	      if (fanout_def_it[2]->to_string() == "sign") {
		sign_track = fanout_def_it[4]->to_int() - 2;
	      } else {
		channel_param.bits_per_sample = 2;
		mag_track = fanout_def_it[4]->to_int() - 2;
	      }
	    }
	    n_bitstream++;
	  }
	  // If there are 64 bitstreams then an input word is 8 bytes long, otherewise is is 4 bytes
	  int word_size = (n_bitstream <= 32) ? 32 : 64;
	  for (int i = 0; i < word_size / n_bitstream;  i++) {
	    int sign = sign_track + i * n_bitstream;
	    channel_param.tracks.push_back(sign);
	    if (channel_param.bits_per_sample == 2) {
	      int magn = mag_track + i * n_bitstream;
	      channel_param.tracks.push_back(magn);
	    }
	  }
	  input_parameters.channels.push_back(channel_param);
	}
      }
      return;
    }
  }

  get_mark5b_standard_mapping(mode, station, input_parameters);
}

void
Control_parameters::
get_vdif_tracks(const std::string &mode,
		const std::string &station,
		const std::string &ds_name,
		Input_node_parameters &input_parameters) const {

  const std::string datastreams_name = get_vex().get_section("DATASTREAMS", mode, station);
  if (get_vex().get_version() > 1.5 || datastreams_name != std::string())
    get_vdif_datastreams(mode, station, ds_name, input_parameters);
  else
    get_vdif_threads(mode, station, input_parameters);
}

void
Control_parameters::
get_vdif_datastreams(const std::string &mode,
		     const std::string &station,
		     const std::string &ds_name,
		     Input_node_parameters &input_parameters) const {

  const std::string datastreams_name = get_vex().get_section("DATASTREAMS", mode, station);
  if (datastreams_name == std::string()) {
    std::cerr << "Cannot find $DATASTREAMS reference for " << station
	      << " in mode" << mode << std::endl;
    sfxc_abort();
  }

  Vex::Node::const_iterator datastream = vex.get_root_node()["DATASTREAMS"][datastreams_name];
  int num_threads = 0;
  input_parameters.frame_size = 0;
  for (Vex::Node::const_iterator thread_it = datastream->begin("thread");
       thread_it != datastream->end("thread"); thread_it++) {
    if (ds_name != thread_it[0]->to_string())
      continue;
    if (input_parameters.frame_size == 0)
      input_parameters.frame_size = thread_it[7]->to_int();
    num_threads++;
  }
  int num_channels = 0;
  for (Vex::Node::const_iterator channel_it = datastream->begin("channel");
       channel_it != datastream->end("channel"); channel_it++) {
    if (ds_name != channel_it[0]->to_string())
      continue;
    num_channels++;
  }

  // We can handle multi-thread, single-channel VDIF in a more
  // efficient way as we don't need to do any unpacking.
  if (num_threads == num_channels) {
      input_parameters.n_tracks = 0;
      for (size_t ch_nr = 0; ch_nr < number_frequency_channels(); ch_nr++) {
	const std::string &channel_name = frequency_channel(ch_nr, mode, station);

	Input_node_parameters::Channel_parameters channel_param;

	std::string thread_name;
	for (Vex::Node::const_iterator channel_it = datastream->begin("channel");
	     channel_it != datastream->end("channel"); channel_it++) {
	  if (channel_name == channel_it[2]->to_string() &&
	      ds_name == channel_it[0]->to_string())
	    thread_name = channel_it[1]->to_string();
	}
	if (thread_name == std::string())
	  continue;

	int thread_id = -1;
	for (Vex::Node::const_iterator thread_it = datastream->begin("thread");
	     thread_it != datastream->end("thread"); thread_it++) {
	  if (ds_name != thread_it[0]->to_string())
	    continue;
	  if (thread_name == thread_it[1]->to_string())
	    thread_id = thread_it[2]->to_int();
	}
	if (thread_id == -1)
	  continue;

	channel_param.bits_per_sample = bits_per_sample(mode, station);
	channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
	channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
	channel_param.frequency_number = frequency_number(ch_nr, mode);
	channel_param.extra_delay_in_samples = extra_delay_in_samples(channel_name, station, mode);
	channel_param.tracks.push_back(thread_id);
	channel_param.tracks.push_back(-1); // XXX
	input_parameters.channels.push_back(channel_param);
      }

      return;
  }

  int num_tracks = 0;
  for (Vex::Node::const_iterator thread_it = datastream->begin("thread");
       thread_it != datastream->end("thread"); thread_it++) {
    if (ds_name != thread_it[0]->to_string())
      continue;
    num_tracks += thread_it[3]->to_int() * thread_it[5]->to_int();
  }

  input_parameters.n_tracks = num_tracks;
  for (size_t ch_nr = 0; ch_nr < number_frequency_channels(); ch_nr++) {
    const std::string &channel_name = frequency_channel(ch_nr, mode, station);

    if (channel_name != std::string()) {
      Input_node_parameters::Channel_parameters channel_param;
      channel_param.bits_per_sample = bits_per_sample(mode, station);
      channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
      channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
      channel_param.frequency_number = frequency_number(ch_nr, mode);
      channel_param.extra_delay_in_samples = extra_delay_in_samples(channel_name, station, mode);

      // NB: Number of channels (and therefore num_tracks) is always a power of two
      const int word_size = (num_tracks <= 32) ? 32 : num_tracks; 
      for (int i = 0; i < word_size; i += num_tracks) {
        for (Vex::Node::const_iterator channel_it = datastream->begin("channel");
             channel_it != datastream->end("channel"); channel_it++) {
	  if (ds_name != channel_it[0]->to_string())
	    continue;
          if (channel_name == channel_it[2]->to_string()) {
            for (int track = bits_per_sample(mode, station) - 1; track >= 0; track--)
              channel_param.tracks.push_back(channel_it[3]->to_int() * bits_per_sample(mode, station) + track + i);
          }
        }
      }
      if (channel_param.tracks.size() > 0)
	input_parameters.channels.push_back(channel_param);
    }
  }
}

void
Control_parameters::
get_vdif_threads(const std::string &mode,
		 const std::string &station,
		 Input_node_parameters &input_parameters) const {

  const std::string threads_name = get_vex().get_section("THREADS", mode, station);
  if (threads_name == std::string()) {
    std::cerr << "Cannot find $THREADS reference for " << station
	      << " in mode" << mode << std::endl;
    sfxc_abort();
  }

  Vex::Node::const_iterator thread = vex.get_root_node()["THREADS"][threads_name];
  int num_threads = 0;
  input_parameters.frame_size = 0;
  for (Vex::Node::const_iterator thread_it = thread->begin("thread");
       thread_it != thread->end("thread"); thread_it++) {
    if (input_parameters.frame_size == 0)
      input_parameters.frame_size = thread_it[8]->to_int();
    num_threads++;
  }
  int num_channels = 0;
  for (Vex::Node::const_iterator channel_it = thread->begin("channel");
       channel_it != thread->end("channel"); channel_it++) {
    num_channels++;
  }

  // We can handle multi-thread, single-channel VDIF in a more
  // efficient way as we don't need to do any unpacking.
  if (num_threads == num_channels) {
      input_parameters.n_tracks = 0;
      for (size_t ch_nr = 0; ch_nr < number_frequency_channels(); ch_nr++) {
	const std::string &channel_name = frequency_channel(ch_nr, mode, station);

        if (channel_name != std::string()) {
	  Input_node_parameters::Channel_parameters channel_param;

	  int thread_id = -1;
	  for (Vex::Node::const_iterator channel_it = thread->begin("channel");
	       channel_it != thread->end("channel"); channel_it++) {
	    if (channel_name == channel_it[0]->to_string())
	      thread_id = channel_it[1]->to_int();
	  }

  	  channel_param.bits_per_sample = bits_per_sample(mode, station);
	  channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
	  channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
	  channel_param.frequency_number = frequency_number(ch_nr, mode);
	  channel_param.extra_delay_in_samples = extra_delay_in_samples(channel_name, station, mode);
	  channel_param.tracks.push_back(thread_id);
	  channel_param.tracks.push_back(-1); // XXX
	  input_parameters.channels.push_back(channel_param);
        }
      }
      return;
  }

  int num_tracks = 0;
  for (Vex::Node::const_iterator channel_it = thread->begin("channel");
       channel_it != thread->end("channel"); channel_it++) {
    num_tracks += bits_per_sample(mode, station);
  }

  input_parameters.n_tracks = num_tracks;
  for (size_t ch_nr = 0; ch_nr < number_frequency_channels(); ch_nr++) {
    const std::string &channel_name = frequency_channel(ch_nr, mode, station);

    if (channel_name != std::string()) {
      Input_node_parameters::Channel_parameters channel_param;
      channel_param.bits_per_sample = bits_per_sample(mode, station);
      channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
      channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
      channel_param.frequency_number = frequency_number(ch_nr, mode);
      channel_param.extra_delay_in_samples = extra_delay_in_samples(channel_name, station, mode);

      // NB: Number of channels (and therefore num_tracks) is always a power of two
      const int word_size = (num_tracks <= 32) ? 32 : num_tracks; 
      for (int i = 0; i < word_size; i += num_tracks) {
        for (Vex::Node::const_iterator channel_it = thread->begin("channel");
             channel_it != thread->end("channel"); channel_it++) {
          if (channel_name == channel_it[0]->to_string()) {
            for (int track = bits_per_sample(mode, station) - 1; track >= 0; track--)
              channel_param.tracks.push_back(channel_it[2]->to_int() * bits_per_sample(mode, station) + track + i);
          }
        }
      }
      input_parameters.channels.push_back(channel_param);
    }
  }
}

void
Control_parameters::
get_mark5b_standard_mapping(const std::string &mode,
                            const std::string &station,
                            Input_node_parameters &input_parameters) const {
 std::cout << RANK_OF_NODE << " : WARNING - No bitstream section for station " <<  station 
                           << ", using standard mapping.\n";
  const Vex::Node &root=get_vex().get_root_node();
  // Find the number of bits per sample
  int bits_per_sample_ = bits_per_sample(mode, station);

  // Get BBC and FREQUENCY nodes
  const std::string bbc = get_vex().get_BBC(mode, station);
  const std::string freq = get_vex().get_frequency(mode, station);
  if(bbc==std::string()){
    char buffer[52];
    snprintf(buffer, 52, "Error : couldn't find BBC section for station %s.", station.c_str());
    sfxc_abort(buffer);
  }
  if(freq==std::string()){
    char buffer[52];
    snprintf(buffer, 52, "Error : couldn't find FREQ section for station %s.", station.c_str());
    sfxc_abort(buffer);
  }
  // subband to bit-stream-nr conversion
  std::map<std::string, int> subband_to_track;
  {
    // Sort the bbc's
    std::map<int, std::string> bbc_map;
    Vex::Node::const_iterator bbc_it;
    for (bbc_it = root["BBC"][bbc]->begin("BBC_assign");
         bbc_it != root["BBC"][bbc]->end("BBC_assign");
         bbc_it ++) {
      bbc_map[bbc_it[1]->to_int()] = bbc_it[0]->to_string();
    }

    // Sorted list of bbc labels
    std::vector<std::string> bbc_labels;
    bbc_labels.resize(bbc_map.size());
    int i=0;
    for (std::map<int, std::string>::iterator it=bbc_map.begin();
         it != bbc_map.end(); it++) {
      bbc_labels[i] = (*it).second;
      i++;
    }

    { // Iterate over bbcs to find the right numbering of the bit streams
      int bit_stream = 0;
      Vex::Node::const_iterator freq_it;
      // Find the upper sidebands:
      for (size_t bbc_nr=0; bbc_nr < bbc_labels.size(); bbc_nr++) {
        for (freq_it = root["FREQ"][freq]->begin("chan_def");
             freq_it != root["FREQ"][freq]->end("chan_def");
             freq_it++) {
          if ((freq_it[2]->to_string() == std::string("U")) &&
              (freq_it[5]->to_string() == bbc_labels[bbc_nr])) {
            subband_to_track[freq_it[4]->to_string()] = bit_stream;
            bit_stream++;
          }
        }
      }
      // Find the lower sidebands:
      for (size_t bbc_nr=0; bbc_nr < bbc_labels.size(); bbc_nr++) {
        for (freq_it = root["FREQ"][freq]->begin("chan_def");
             freq_it != root["FREQ"][freq]->end("chan_def");
             freq_it++) {
          if ((freq_it[2]->to_string() == std::string("L")) &&
              (freq_it[5]->to_string() == bbc_labels[bbc_nr])) {
            subband_to_track[freq_it[4]->to_string()] = bit_stream;
            bit_stream++;
          }
        }
      }
    }
  }
  // Total number of bitstreams according to vex file
  input_parameters.n_tracks = subband_to_track.size() * bits_per_sample_;

  { // Fill the sign and magnitude bits:
    int nr_bit_streams = subband_to_track.size()*bits_per_sample_;
    for (size_t ch_nr=0; ch_nr < number_frequency_channels(); ch_nr++) {
      const std::string &channel_name = frequency_channel(ch_nr, mode, station);
      int bit_stream_nr = subband_to_track[channel_name]*bits_per_sample_;

      if (channel_name != std::string()) {
        Input_node_parameters::Channel_parameters channel_param;
        channel_param.bits_per_sample = bits_per_sample_;
	channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
	channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
        channel_param.frequency_number = frequency_number(ch_nr, mode);
        channel_param.extra_delay_in_samples = extra_delay_in_samples(channel_name, station, mode);
        if (bits_per_sample_ == 2) {
          for (; bit_stream_nr < 32; bit_stream_nr += nr_bit_streams) {
            channel_param.tracks.push_back(bit_stream_nr);
            channel_param.tracks.push_back(bit_stream_nr+1);
          }
        } else {
          for (; bit_stream_nr < 32; bit_stream_nr += nr_bit_streams) {
            channel_param.tracks.push_back(bit_stream_nr);
          }
        }
        input_parameters.channels.push_back(channel_param);
      }
    }
  }
}

Input_node_parameters
Control_parameters::
get_input_node_parameters(const std::string &mode_name,
                          const std::string &station_name,
			  const std::string &ds_name) const {
  Input_node_parameters result;
  result.track_bit_rate = -1;
  result.frame_size = -1;
  result.offset = reader_offset(station_name);
  result.overlap_time =  0;
  result.phasecal_integr_time = phasecal_integration_time();
  result.exit_on_empty_datastream = exit_on_empty_datastream();

  const Vex::Node &root = vex.get_root_node();
  Vex::Node::const_iterator mode = root["MODE"][mode_name];
  if (mode == root["MODE"]->end()) {
    std::cerr << "Cannot find mode " << mode_name << std::endl;
    sfxc_abort();
  }
  const std::string &freq_name = vex.get_frequency(mode_name, station_name);
  if (freq_name == std::string()) {
    std::cerr << "Cannot find $FREQ reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["FREQ"][freq_name] == root["FREQ"]->end()) {
    std::cerr << "Cannot find " << freq_name << " in $FREQ block" << std::endl;
    sfxc_abort();
  }

  const std::string &if_name = vex.get_IF(mode_name, station_name);
  if (if_name == std::string()) {
    std::cerr << "Cannot find $IF reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["IF"][if_name] == root["IF"]->end()) {
    std::cerr << "Cannot find " << if_name << " in $IF block" << std::endl;
    sfxc_abort();
  }

  const std::string &bbc_name = vex.get_BBC(mode_name, station_name);
  if (bbc_name == std::string()) {
    std::cerr << "Cannot find $BBC reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["BBC"][bbc_name] == root["BBC"]->end()) {
    std::cerr << "Cannot find " << bbc_name << " in $BBC block" << std::endl;
    sfxc_abort();
  }

  result.track_bit_rate = sample_rate(mode_name, station_name);

  if (data_format(station_name, mode_name) == "VDIF") {
    get_vdif_tracks(mode_name, station_name, ds_name, result);
  } else if (data_format(station_name, mode_name) == "Mark4" ||
	     data_format(station_name, mode_name) == "VLBA")  {
    get_mark5a_tracks(mode_name, station_name, result);
  } else {
    SFXC_ASSERT(data_format(station_name, mode_name) == "Mark5B");
    get_mark5b_tracks(mode_name, station_name, result);
  }

  if (!result.channels.empty()) {
    SFXC_ASSERT(!result.channels[0].tracks.empty());
    result.track_bit_rate /= result.channels[0].tracks.size() / result.channels[0].bits_per_sample;
  }
  return result;
}

std::string
Control_parameters::data_format(const std::string &station, const std::string &mode) const {
// For Vex 1.5 data_format is determined using a heuristic through the DAS section.
// For Vex 2.0 the existance of either a DATASTREAMS (VDIF), BITSTREAMS (Mark5B), 
// or TRACKS (Mark5a) block determines the data format.
  if (get_vex().get_version() > 1.5) {
    if (get_vex().get_section("DATASTREAMS", mode, station) != std::string())
      return "VDIF";
     
    if (get_vex().get_section("BITSTREAMS", mode, station) != std::string())
      return "Mark5B";
      
    std::string tracks_name = get_vex().get_section("TRACKS", mode, station);
    if (tracks_name != std::string()) {
      Vex::Node::const_iterator tracks = get_vex().get_root_node()["TRACKS"][tracks_name];
      return tracks["track_frame_format"]->to_string();
    }
  } else {
    if (recorder_type(station) == "Mark5A") {
      if (rack_type(station) == "VLBA4")
        return "Mark4";
      else
        return rack_type(station);
    }
    if (recorder_type(station) == "Mark5B") {
      if (rack_type(station) == "DVP" || rack_type(station) == "RDBE2" ||
          rack_type(station) == "WIDAR")
        return "VDIF";
      else
        return "Mark5B";
    }
    if (recorder_type(station) == "Mark5C") {
      if (rack_type(station) == "DBBC" || rack_type(station) == "DVP" ||
          rack_type(station) == "RDBE2" || rack_type(station) == "WIDAR")
        return "VDIF";
    }
    if (recorder_type(station) == "Mark6") {
      return "VDIF";
    }
    if (recorder_type(station) == "None") {
      if (rack_type(station) == "DBBC")
        return "VDIF";
    }
  }
  std::cerr << "Cannot determine data format for " << station << std::endl;
  sfxc_abort();
}

std::string
Control_parameters::rack_type(const std::string &station) const {
  const Vex::Node &root = vex.get_root_node();
  Vex::Node::const_iterator station_block = root["STATION"][station];
  for (Vex::Node::const_iterator das_it = station_block->begin("DAS");
       das_it != station_block->end("DAS"); ++das_it) {
    const std::string das_name = das_it->to_string();
    if (root["DAS"][das_name] == root["DAS"]->end()) {
      std::cerr << "Cannot find " << das_name << " in $DAS block" << std::endl;
      sfxc_abort();
    }
    Vex::Node::const_iterator das = root["DAS"][das_name];
    if (das["equip"] != das->end()) {
      for (Vex::Node::const_iterator equip_it = das->begin("equip");
	   equip_it != das->end("equip"); equip_it++) {
	if (equip_it[0]->to_string() == "rack")
	  return equip_it[1]->to_string();
      }
    }
    if (vex.get_version() <= 1.5 && das["electronics_rack_type"] != das->end())
      return das["electronics_rack_type"]->to_string();
  }
  return std::string();
}

std::string
Control_parameters::recorder_type(const std::string &station) const {
  const Vex::Node &root = vex.get_root_node();
  Vex::Node::const_iterator station_block = root["STATION"][station];
  for (Vex::Node::const_iterator das_it = station_block->begin("DAS");
       das_it != station_block->end("DAS"); ++das_it) {
    const std::string das_name = das_it->to_string();
    if (root["DAS"][das_name] == root["DAS"]->end()) {
      std::cerr << "Cannot find " << das_name << " in $DAS block" << std::endl;
      sfxc_abort();
    }
    Vex::Node::const_iterator das = root["DAS"][das_name];
    if (das["equip"] != das->end()) {
      for (Vex::Node::const_iterator equip_it = das->begin("equip");
	   equip_it != das->end("equip"); equip_it++) {
	if (equip_it[0]->to_string() == "recorder")
	  return equip_it[1]->to_string();
      }
    }
    if (vex.get_version() <= 1.5 && das["record_transport_type"] != das->end())
      return das["record_transport_type"]->to_string();
  }
  return std::string();
}

bool
Control_parameters::cross_polarize() const {
  if (!ctrl["cross_polarize"].asBool())
    return false;
  for (Vex::Node::const_iterator mode_it =
         vex.get_root_node()["MODE"]->begin();
       mode_it != vex.get_root_node()["MODE"]->end();
       ++mode_it) {
    for (size_t ch_nr=0; ch_nr<number_frequency_channels(); ch_nr++) {
      if (cross_channel(ch_nr, mode_it.key()) != -1)
        return true;
    }
  }
  return false;
}

int
Control_parameters::
cross_channel(int channel_nr, const std::string &mode) const {
  if (channel_nr >= (int)number_frequency_channels())
    return -1;
  return cross_channel(channel(channel_nr), mode);
}

int
Control_parameters::
cross_channel(const std::string &channel_name,
              const std::string &mode) const {
  std::string freq = frequency(channel_name, setup_station(), mode);
  if (freq != std::string()){
    char side = sideband(channel_name, setup_station(), mode);
    char pol  = polarisation(channel_name, setup_station(), mode);
    if(pol != ' '){
      for (size_t i = 0; i < number_frequency_channels(); i++) {
        if (channel(i) != channel_name) {
	  if ((freq == frequency(channel(i), setup_station(), mode)) &&
	      (side == sideband(channel(i), setup_station(), mode)) &&
	      (pol != polarisation(channel(i), setup_station(), mode))) {
  	    return i;
  	  } 
        }
      }
    }
  }
  return -1;
}

char
Control_parameters::
polarisation(const std::string &channel_name,
             const std::string &station_name,
             const std::string &mode_name) const {
  const Vex::Node &root = vex.get_root_node();
  Vex::Node::const_iterator mode = root["MODE"][mode_name];
  if (mode == root["MODE"]->end()) {
    std::cerr << "Cannot find mode " << mode_name << std::endl;
    sfxc_abort();
  }
  const std::string &freq_name = vex.get_frequency(mode_name, station_name);
  if (freq_name == std::string()) {
    std::cerr << "Cannot find $FREQ reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["FREQ"][freq_name] == root["FREQ"]->end()) {
    std::cerr << "Cannot find " << freq_name << " in $FREQ block" << std::endl;
    sfxc_abort();
  }

  const std::string &if_name = vex.get_IF(mode_name, station_name);
  if (if_name == std::string()) {
    std::cerr << "Cannot find $IF reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["IF"][if_name] == root["IF"]->end()) {
    std::cerr << "Cannot find " << if_name << " in $IF block" << std::endl;
    sfxc_abort();
  }

  const std::string &bbc_name = vex.get_BBC(mode_name, station_name);
  if (bbc_name == std::string()) {
    std::cerr << "Cannot find $BBC reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["BBC"][bbc_name] == root["BBC"]->end()) {
    std::cerr << "Cannot find " << bbc_name << " in $BBC block" << std::endl;
    sfxc_abort();
  }

  std::string bbc_ref;
  Vex::Node::const_iterator freq = root["FREQ"][freq_name];
  for (Vex::Node::const_iterator chan = freq->begin("chan_def");
       chan != freq->end("chan_def"); chan++) {
    if (chan[4]->to_string() == channel_name)
      bbc_ref = chan[5]->to_string();
  }

  std::string if_ref;
  Vex::Node::const_iterator bbc = root["BBC"][bbc_name];
  for (Vex::Node::const_iterator bbc_it = bbc->begin("BBC_assign");
       bbc_it != bbc->end("BBC_assign"); bbc_it++) {
    if (bbc_it[0]->to_string() == bbc_ref)
      if_ref = bbc_it[2]->to_string();
  }

  return vex.polarisation(if_name, if_ref);
}

int
Control_parameters::
polarisation_type_for_global_output_header(const std::string &mode) const {
  if (cross_polarize())
    return Output_header_global::LEFT_RIGHT_POLARISATION_WITH_CROSSES;

  bool left = false, right = false;
  // Assume station 0 is in all scans
  std::string station_name = setup_station();
  for (size_t ch_nr=0; ch_nr<number_frequency_channels(); ch_nr++) {
    std::string channel_name = frequency_channel(ch_nr, mode, station_name);
    if (channel_name != std::string()){
      if (channel_name != std::string()){
        char pol = polarisation(channel_name, station_name, mode);
        if (std::toupper(pol) == 'L')
          left = true;
        else if (std::toupper(pol) == 'R')
          right = true;
      }
    }
  }
  if (left && right)
    return Output_header_global::LEFT_RIGHT_POLARISATION;
  if (left)
    return Output_header_global::LEFT_POLARISATION;

  SFXC_ASSERT(right);
  return Output_header_global::RIGHT_POLARISATION;
}

std::string
Control_parameters::
frequency(const std::string &channel_name,
          const std::string &station_name,
          const std::string &mode_name) const {
  std::string freq_name;

  Vex::Node::const_iterator mode = vex.get_root_node()["MODE"][mode_name];
  for (Vex::Node::const_iterator freq_it = mode->begin("FREQ");
       freq_it != mode->end("FREQ"); freq_it++) {
    for (Vex::Node::const_iterator elem_it = freq_it->begin();
         elem_it != freq_it->end(); elem_it++) {
      if (elem_it->to_string() == station_name) {
        freq_name = freq_it[0]->to_string();
      }
    }
  }

  if (freq_name != std::string()){
    Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];
    for (Vex::Node::const_iterator ch_it = freq->begin("chan_def");
         ch_it != freq->end("chan_def"); ch_it++) {
      if (ch_it[4]->to_string() == channel_name) {
        return ch_it[1]->to_string();
      }
    }
  }

  return std::string();
}

char
Control_parameters::
sideband(const std::string &channel_name,
         const std::string &station_name,
         const std::string &mode) const {

  std::string if_mode_freq;
  std::string if_node_Node;
  std::string if_ref_BBC;
  std::string if_ref_BBCnr;
  std::string if_ref_Ref;
  char sband = 'x';

  Vex::Node::const_iterator mode_block = vex.get_root_node()["MODE"][mode];
  for (Vex::Node::const_iterator if_it = mode_block->begin("FREQ");
       if_it != mode_block->end("FREQ"); ++if_it) {
    for (Vex::Node::const_iterator elem_it = if_it->begin();
         elem_it != if_it->end(); ++elem_it) {
      if (elem_it->to_string() == station_name) {
        if_mode_freq = if_it[0]->to_string();
      }
    }
  }
  for (Vex::Node::const_iterator if_it = mode_block->begin("IF");
       if_it != mode_block->end("IF"); ++if_it) {
    for (Vex::Node::const_iterator elem_it = if_it->begin();
         elem_it != if_it->end(); ++elem_it) {
      if (elem_it->to_string() == station_name) {
        if_node_Node = if_it[0]->to_string();
      }
    }
  }
  for (Vex::Node::const_iterator bbc_it = mode_block->begin("BBC");
       bbc_it != mode_block->end("BBC"); ++bbc_it) {
    for (size_t i=1; i<bbc_it->size(); i++) {
      if (bbc_it[i]->to_string() == station_name) {
        if_ref_BBC = bbc_it[0]->to_string();
      }
    }
  }


  for (Vex::Node::const_iterator frq_block = vex.get_root_node()["FREQ"][if_mode_freq]->begin("chan_def");
       frq_block != vex.get_root_node()["FREQ"][if_mode_freq]->end("chan_def"); ++frq_block) {
    for (Vex::Node::const_iterator elem_it = frq_block->begin();
         elem_it != frq_block->end(); ++elem_it) {
      if (elem_it->to_string() == channel_name) {
        sband = frq_block[2]->to_char();
      }
    }
  }

  return sband;
}

int
Control_parameters::station_number(const std::string &station_name) const
{
  if (station_map.empty()) {
    for (Vex::Node::const_iterator station_it =
	   vex.get_root_node()["STATION"]->begin();
	 station_it != vex.get_root_node()["STATION"]->end(); ++station_it) {
      station_map[station_it.key()] = -1;
    }

    int station_number = 0;
    for (std::map<std::string, int>::iterator it = station_map.begin();
	 it != station_map.end(); it++) {
      it->second = station_number;
      station_number++;
    }
  }

  return station_map[station_name];
}

Correlation_parameters
Control_parameters::
get_correlation_parameters(const std::string &scan_name,
			   size_t channel_nr,
                           int integration_nr,
                           const std::map<stream_key, int> &correlator_node_station_to_input) const {
  std::string bbc_nr;
  std::string bbc_mode;
  std::string if_nr;
  std::string if_mode;

  Vex::Node::const_iterator scan =
    vex.get_root_node()["SCHED"][scan_name];
  std::string mode_name = scan["mode"]->to_string();
  Vex::Node::const_iterator mode =
    vex.get_root_node()["MODE"][mode_name];

  const std::string &station_name = setup_station();
  const std::string &channel_name =
    frequency_channel(channel_nr, mode_name, station_name);

  Correlation_parameters corr_param;
  corr_param.experiment_start = vex.get_start_time_of_experiment();
  corr_param.integration_time = integration_time();
  corr_param.slice_time = corr_param.integration_time / slices_per_integration();
  corr_param.sub_integration_time = sub_integration_time(); 
  corr_param.number_channels = number_channels();
  corr_param.fft_size_delaycor = fft_size_delaycor();
  corr_param.fft_size_correlation = fft_size_correlation();
  corr_param.window = window_function();  
  corr_param.sample_rate = sample_rate(mode_name, station_name);

  corr_param.sideband = ' ';
  const std::string &freq_name = get_vex().get_frequency(mode_name, station_name);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];
  for (Vex::Node::const_iterator ch_it = freq->begin("chan_def");
       ch_it != freq->end("chan_def");
       ++ch_it) {
    if (ch_it[4]->to_string() == channel_name) {
      corr_param.channel_freq = (int64_t)round(ch_it[1]->to_double_amount("Hz"));
      corr_param.bandwidth = (uint64_t)ch_it[3]->to_double_amount("Hz");
      corr_param.sideband = ch_it[2]->to_char();
      bbc_nr = ch_it[5]->to_string();
    }
  }
  corr_param.frequency_nr = frequency_number(channel_nr, mode_name);

  //in the following two blocks (if_mode and bbc_mode) we assume only one of the
  //station name HO
  for (Vex::Node::const_iterator if_it = mode->begin("IF");
       if_it != mode->end("IF"); ++if_it) {
    for (Vex::Node::const_iterator elem_it = if_it->begin();
         elem_it != if_it->end(); ++elem_it) {
      if (elem_it->to_string() == station_name) {
        if_mode = if_it[0]->to_string();
      }
    }
  }
  for (Vex::Node::const_iterator bbc_it = mode->begin("BBC");
       bbc_it != mode->end("BBC"); ++bbc_it) {
    for (size_t i=1; i<bbc_it->size(); i++) {
      if (bbc_it[i]->to_string() == station_name) {
        bbc_mode = bbc_it[0]->to_string();
      }
    }
  }

  for (Vex::Node::const_iterator bbc_block = vex.get_root_node()["BBC"][bbc_mode]->begin();
       bbc_block != vex.get_root_node()["BBC"][bbc_mode]->end(); ++bbc_block) {
    for (Vex::Node::const_iterator bbcnr_it = bbc_block->begin();
         bbcnr_it != bbc_block->end(); ++bbcnr_it) {
      if (bbcnr_it->to_string() == bbc_nr) {
        if_nr = bbc_block[2]->to_string();
      }
    }
  }

  corr_param.polarisation = vex.polarisation(if_mode, if_nr);

  SFXC_ASSERT(corr_param.sideband == 'L' || corr_param.sideband == 'U');

  corr_param.cross_polarize = cross_polarize();
  if (cross_channel(channel_name, mode_name) == -1) {
    corr_param.cross_polarize = false;
  }

  corr_param.reference_station = reference_station_number();

  std::set<int> stations_set;
  for (Vex::Node::const_iterator station = scan->begin("station");
       station != scan->end("station"); ++station) {
    const std::string &channel_name =
      frequency_channel(channel_nr, mode_name, station[0]->to_string());
    const std::string ds_name =
      datastream(mode_name, station[0]->to_string(), channel_name);
    std::map<stream_key, int>::const_iterator station_nr_it =
      correlator_node_station_to_input.find(stream_key(station[0]->to_string(), ds_name));
    if (station_nr_it != correlator_node_station_to_input.end()) {
      if (station_nr_it->second >= 0) {
        if (channel_name != std::string()) {
          Correlation_parameters::Station_parameters station_param;
          station_param.station_number = station_number(station[0]->to_string());
          stations_set.insert(station_param.station_number);
          station_param.station_stream = station_nr_it->second;
          station_param.bits_per_sample = bits_per_sample(mode_name, station[0]->to_string());
          station_param.sample_rate = sample_rate(mode_name, station[0]->to_string());
          station_param.channel_freq = channel_freq(mode_name, station[0]->to_string(), channel_name);
          station_param.bandwidth = bandwidth(mode_name, station[0]->to_string(), channel_name);
          station_param.sideband = sideband(channel_name, station[0]->to_string(), mode_name);
          station_param.polarisation = polarisation(channel_name, station[0]->to_string(), mode_name);
          station_param.LO_offset = LO_offset(station[0]->to_string(), integration_nr);
          station_param.extra_delay = extra_delay(channel_name, station[0]->to_string(), mode_name);
          station_param.tsys_freq = tsys_freq(station[0]->to_string());
          corr_param.station_streams.push_back(station_param);
        }
      }
    }
  }

  int nfft = nr_correlation_ffts_per_integration(corr_param.slice_time,
                                           corr_param.sample_rate,
                                           fft_size_correlation());
  corr_param.slice_size = fft_size_correlation() * nfft;
  // Round slice time down to an integral number of FFTs
  corr_param.slice_time = Time((1000000ULL * corr_param.slice_size) / corr_param.sample_rate);

  if (!corr_param.cross_polarize)
    return corr_param;

  channel_nr = cross_channel(channel_nr, mode_name);
  for (Vex::Node::const_iterator station = scan->begin("station");
       station != scan->end("station"); ++station) {
    const std::string &channel_name =
      frequency_channel(channel_nr, mode_name, station[0]->to_string());
    const std::string ds_name =
      datastream(mode_name, station[0]->to_string(), channel_name);
    std::map<stream_key, int>::const_iterator station_nr_it =
      correlator_node_station_to_input.find(stream_key(station[0]->to_string(), ds_name));
    if (station_nr_it != correlator_node_station_to_input.end()) {
      if (station_nr_it->second >= 0) {
	const std::string &channel_name =
	  frequency_channel(channel_nr, mode_name, station[0]->to_string());
        if (channel_name != std::string()) {
          Correlation_parameters::Station_parameters station_param;
          station_param.station_number = station_number(station[0]->to_string());
	  if (stations_set.count(station_param.station_number) > 0)
	    station_param.station_stream = station_nr_it->second + number_inputs();
	  else
	    station_param.station_stream = station_nr_it->second;;
          station_param.bits_per_sample = bits_per_sample(mode_name, station[0]->to_string());
          station_param.sample_rate = sample_rate(mode_name, station[0]->to_string());
          station_param.channel_freq = channel_freq(mode_name, station[0]->to_string(), channel_name);
          station_param.bandwidth = bandwidth(mode_name, station[0]->to_string(), channel_name);
          station_param.sideband = sideband(channel_name, station[0]->to_string(), mode_name);
          station_param.polarisation = polarisation(channel_name, station[0]->to_string(), mode_name);
          station_param.LO_offset = LO_offset(station[0]->to_string(), integration_nr);
          station_param.extra_delay = extra_delay(channel_name, station[0]->to_string(), mode_name);
          station_param.tsys_freq = tsys_freq(station[0]->to_string());
          corr_param.station_streams.push_back(station_param);
        }
      }
    }
  }

  return corr_param;
}

std::string
Control_parameters::
get_delay_table_name(const std::string &station_name) const {
  if (strncmp(ctrl["delay_directory"].asString().c_str(),  "file://", 7) != 0)
    sfxc_abort("Ctrl-file: Delay directory doesn't start with 'file://'");
  std::string delay_table_name;
  if (ctrl["delay_directory"].asString().size()==7)
    // delay files are in the current directory
    delay_table_name = get_exper_name() + "_" +station_name + ".del";
  else
    delay_table_name = std::string(ctrl["delay_directory"].asString().c_str()+7) +
      "/" + get_exper_name() + "_" + station_name + ".del";

  if (access(delay_table_name.c_str(), R_OK) == 0)
    return delay_table_name;
  generate_delay_table(station_name, delay_table_name);
  if (access(delay_table_name.c_str(), R_OK) == 0)
    return delay_table_name;
  DEBUG_MSG("Tried to create the delay table at " << delay_table_name);
  sfxc_abort("Couldn't create the delay table.");
  return std::string("");
}

void
Control_parameters::
generate_delay_table(const std::string &station_name,
                     const std::string &filename) const {
  std::string cmd =
    "generate_delay_model "+vex_filename+" "+station_name+" "+filename;
  DEBUG_MSG("Creating the delay model: " << cmd);
  int result = system(cmd.c_str());
  if (result != 0) {
    sfxc_abort("Generation of the delay table failed (generate_delay_model)");
  }
}

std::string
Control_parameters::create_path(const std::string &path) const {
  if (strncmp(path.c_str(), "file://", 7) == 0) {
    if (path[7] != '/') {
      std::string result = "file://";
      char c_ctrl_filename[ctrl_filename.size()+1];
      strcpy(c_ctrl_filename, ctrl_filename.c_str());
      result += dirname(c_ctrl_filename);
      result += "/";
      result += path.c_str()+7;
      return result;
    } else {
      return path;
    }
  } else {
    return path;
  }
}

bool
Input_node_parameters::operator==(const Input_node_parameters &other) const {
  if (channels != other.channels)
    return false;
  if (track_bit_rate != other.track_bit_rate)
    return false;

  return true;
}

std::ostream &
operator<<(std::ostream &out,
           const Input_node_parameters &param) {
  out << "{ \"n_tracks\": " << param.n_tracks << ", "
      <<"\"track_bit_rate\": " << param.track_bit_rate << ", "
      << std::endl;

  out << " channels: [";
  for (size_t i=0; i<param.channels.size(); i++) {
    if (i > 0)
      out << ",";
    out << std::endl;
    int bps = param.channels[i].bits_per_sample;
    for (size_t track = 0; track < param.channels[i].tracks.size(); track+=bps) {
      if (track > 0)
        out << ", ";
      out << param.channels[i].tracks[track];
    }
    out << "] ], ";
    if(bps == 2){
      for (size_t track = 1; track < param.channels[i].tracks.size(); track+=bps) {
        if (track > 0)
          out << ", ";
        out << param.channels[i].tracks[track];
      }
    }
    out << "] ] }";
  }
  out << "] }" << std::endl;

  return out;
}

int
Input_node_parameters::bits_per_sample() const {
  SFXC_ASSERT(!channels.empty());
  for (Channel_const_iterator it=channels.begin();
       it!=channels.end(); it++) {
    SFXC_ASSERT(channels.begin()->bits_per_sample ==
                it->bits_per_sample);
  }
  return channels.begin()->bits_per_sample;
}

int
Input_node_parameters::subsamples_per_sample() const {
  SFXC_ASSERT(!channels.empty());
  for (Channel_const_iterator it=channels.begin();
       it!=channels.end(); it++) {
    SFXC_ASSERT(channels.begin()->tracks.size() ==
                it->tracks.size());
  }
  return channels.begin()->tracks.size() / channels.begin()->bits_per_sample;
}

uint64_t
Input_node_parameters::sample_rate() const {
  return track_bit_rate * subsamples_per_sample();
}

bool
Input_node_parameters::Channel_parameters::
operator==(const Input_node_parameters::Channel_parameters &other) const {
  if (tracks != other.tracks)
    return false;
  if (bits_per_sample != other.bits_per_sample)
    return false;

  return true;
}

bool
Correlation_parameters::operator==(const Correlation_parameters& other) const {
  if (slice_start != other.slice_start)
    return false;
  if (slice_time != other.slice_time)
    return false;
  if (integration_start != other.integration_start)
    return false;
  if (integration_time != other.integration_time)
    return false;
  if (stream_start != other.stream_start)
    return false;
  if (slice_size != other.slice_size)
        return false;
  if (number_channels != other.number_channels)
    return false;
  if (fft_size_delaycor != other.fft_size_delaycor)
    return false;
  if (fft_size_correlation != other.fft_size_correlation)
    return false;
  if (window != other.window)
    return false;
  if (integration_nr != other.integration_nr)
    return false;
  if (slice_nr != other.slice_nr)
    return false;

  if (sample_rate != other.sample_rate)
    return false;

  if (channel_freq != other.channel_freq)
    return false;
  if (bandwidth != other.bandwidth)
    return false;
  if (sideband != other.sideband)
    return false;

  if (station_streams != other.station_streams)
    return false;
  return true;
}

std::ostream &operator<<(std::ostream &out,
                         const Correlation_parameters &param) {
  out << "{ ";
  out << "\"slice_start\": " << param.slice_start << ", " << std::endl;
  out << "  \"stream_start\": " << param.stream_start << ", " << std::endl;
  out << "  \"slice size\": " << param.slice_size << ", " << std::endl;
  out << "  \"integr_time\": " << param.integration_time << ", " << std::endl;
  out << "  \"number_channels\": " << param.number_channels << ", " << std::endl;
  out << "  \"fft_size_delaycor\": " << param.fft_size_delaycor << ", " << std::endl;
  out << "  \"fft_size_correlation\": " << param.fft_size_correlation << ", " << std::endl;
  out << "  \"window\": " << param.window << ", " << std::endl;
  out << "  \"slice_nr\": " << param.slice_nr << ", " << std::endl;
  out << "  \"sample_rate\": " << param.sample_rate << ", " << std::endl;
  out << "  \"channel_freq\": " << param.channel_freq << ", " << std::endl;
  out << "  \"bandwidth\": " << param.bandwidth<< ", " << std::endl;
  out << "  \"sideband\": " << param.sideband << ", " << std::endl;
  out << "  \"cross_polarize\": " << (param.cross_polarize ? "true" : "false")<< ", " << std::endl;
  out << "  \"reference_station\": " << param.reference_station << ", " << std::endl;
  out << "  \"station_streams\": [";
  for (size_t i=0; i<param.station_streams.size(); i++) {
    if (i!=0)
      out << ", ";
    out << std::endl;
    out << "{ \"stream\": " <<param.station_streams[i].station_stream
    << ", \"bits_per_sample\": " << param.station_streams[i].bits_per_sample
    << ", \"sample_rate\": " << param.station_streams[i].sample_rate
    << ", \"bandwidth\": " << param.station_streams[i].bandwidth
    << "  \"channel_freq\": " << param.station_streams[i].channel_freq
    << "  \"sideband\": " << param.station_streams[i].sideband
    << " }";
  }
  out << "] }" << std::endl;
  return out;
}

bool
Correlation_parameters::Station_parameters::
operator==(const Correlation_parameters::Station_parameters& other) const {
  if (station_number != other.station_number)
    return false;
  if (station_stream != other.station_stream)
    return false;
  return true;
}

Pulsar_parameters::Pulsar_parameters(std::ostream& log_writer_):log_writer(log_writer_){
}

bool 
Pulsar_parameters::parse_polyco(std::vector<Polyco_params> &param, std::string filename){
  bool polyco_completed = false, read_error = false;
  std::ifstream inp(filename.c_str());
  std::string line, temp;

  if(!inp){
    log_writer << "Could not open polyco file [" <<filename<<"]\n";
    return false;
  }
  int line_nr=0;
  int coef_idx=0;
  int n_coef=0;
  int block_index=0;
  int end_of_prev_block=0;
  param.resize(0);
  std::getline(inp, line);
  while(!inp.eof()){
    std::stringstream inpline(line);
    if(line_nr-end_of_prev_block==0){
      inpline >> temp;
      param.resize(block_index+1);
      strncpy(param[block_index].name,temp.c_str(),11);
      param[block_index].name[10]=0; // make sure sting is null terminated
      inpline >> temp;
      strncpy(param[block_index].date,temp.c_str(),10);
      param[block_index].date[9]=0; // make sure sting is null terminated
      inpline >> param[block_index].utc;
      inpline >> param[block_index].tmid;
      inpline >> param[block_index].DM;
      inpline >> param[block_index].doppler;
      inpline >> param[block_index].residual;

      polyco_completed = false;
      read_error = inpline.fail();
    }else if(line_nr-end_of_prev_block == 1){
      inpline >> param[block_index].ref_phase;
      inpline >> param[block_index].ref_freq;
      inpline >> temp;
      strncpy(param[block_index].site, temp.c_str(), 6);
      param[block_index].site[5]=0; // make sure sting is null terminated
      inpline >> param[block_index].data_span;
      inpline >> param[block_index].n_coef;
      n_coef = param[block_index].n_coef;
      param[block_index].coef.resize(n_coef);
      inpline >> param[block_index].obs_freq;
      read_error = inpline.fail();
      // The binary phase parameters are optional
      inpline >> param[block_index].bin_phase[0];
      if(!inpline.fail()){
        inpline >> param[block_index].bin_phase[1];
        read_error = inpline.fail();
      }else{
        param[block_index].bin_phase[0]=0;
        param[block_index].bin_phase[1]=0;
      }
    }else{
      while((!inpline.eof())&&(!inpline.fail())&&(coef_idx<n_coef)){
        inpline >> param[block_index].coef[coef_idx];
        coef_idx++;
      }
      if((!inpline.fail())&&(coef_idx == n_coef)){
        polyco_completed = true;
        block_index++;
        coef_idx=0;
        end_of_prev_block=line_nr+1;
      }
      read_error = inpline.fail();
    }
    if(read_error){
      log_writer << " Error parsing line " << line_nr + 1 << " of polyco file [" << filename << "]\n";
      return false;
    }
    line_nr++;
    std::getline(inp, line);
  }
  if(!polyco_completed)
    log_writer << " Eof reached prematurely while parsing polyco file [" << filename << "]\n";

  return polyco_completed;
}
