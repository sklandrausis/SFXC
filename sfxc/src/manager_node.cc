/* Copyright (c) 2007 Joint Institute for VLBI in Europe (Netherlands)
 * All rights reserved.
 *
 * Author(s): Nico Kruithof <Kruithof@JIVE.nl>, 2007
 *
 * $Id$
 *
 */

#include "manager_node.h"
#include "sfxc_mpi.h"
#include "utils.h"
#include "mpi_transfer.h"
#include "log_writer_cout.h"
#include "uvw_model.h"
#include "svn_version.h"

#include <fstream>
#include <iostream>
#include <iomanip>
#include <fftw3.h>
#include <stdlib.h>
#include <cstring>
#include <set>

Manager_node::
Manager_node(int rank, int numtasks,
             Log_writer *log_writer,
             const Control_parameters &control_parameters)
    : Abstract_manager_node(rank, numtasks,
                            log_writer,
                            control_parameters),
    manager_controller(*this),
    integration_nr(0),
    slice_nr(0),
    current_scan(0)
/**/ {
  SFXC_ASSERT(rank == RANK_MANAGER_NODE);

  add_controller(&manager_controller);

  get_log_writer()(1) << "Starting nodes" << std::endl;

  // initialise the log node
  //start_log_node(RANK_LOG_NODE, "file://./output.txt");
  start_log_node(RANK_LOG_NODE);

  // initialise the output node
  start_output_node(RANK_OUTPUT_NODE);

  // Input nodes:
  int n_inputs = get_control_parameters().number_inputs();
  int station_number = 0;
  int datastream_number = 0;
  for (int input_node = 0; input_node < n_inputs; input_node++) {
    int input_rank = input_node + 3;
    SFXC_ASSERT(input_rank != RANK_MANAGER_NODE);
    SFXC_ASSERT(input_rank != RANK_LOG_NODE);
    SFXC_ASSERT(input_rank != RANK_OUTPUT_NODE);
    SFXC_ASSERT(input_rank < numtasks);

    station_map[input_node] = station_number; /* XXX 1:1 for now */
    const std::string station = get_control_parameters().station(station_number);
    const std::vector<std::string> datastreams = get_control_parameters().datastreams(station);
    datastream_map[input_node] = datastreams[datastream_number++];

    start_input_node(input_rank, station, datastream_map[input_node]);

    if (datastream_number >= datastreams.size()) {
      station_number++;
      datastream_number = 0;
    }
  }
  SFXC_ASSERT(n_inputs > 0);

  // correlator nodes:
  int mintasks = 3 + n_inputs + control_parameters.number_correlation_cores_per_timeslice(get_current_mode());
  SFXC_ASSERT (numtasks >= mintasks);

  n_corr_nodes = numtasks - (n_inputs + 3);
  std::vector<MPI_Request> pending_requests;
  int numrequest;

  if (control_parameters.cross_polarize()) {
    numrequest = (n_inputs * 2 + 1) * n_corr_nodes;
  } else {
    numrequest = (n_inputs + 1) * n_corr_nodes;
  }
  pending_requests.resize(numrequest);
  int currreq = 0;
  for (int correlator_nr = 0; correlator_nr < n_corr_nodes; correlator_nr++) {
    int correlator_rank = correlator_nr + n_inputs + 3;
    SFXC_ASSERT(correlator_rank != RANK_MANAGER_NODE);
    SFXC_ASSERT(correlator_rank != RANK_LOG_NODE);
    SFXC_ASSERT(correlator_rank != RANK_OUTPUT_NODE);

    start_correlator_node(correlator_rank);

    // Set up the connection to the input nodes:
    for (int input_node = 0; input_node < n_inputs; input_node++) {
      int input_rank = input_node + 3;
      connect_to(input_rank,
		 correlator_nr,
		 correlator_rank, input_node,
		 input_node_cnx_params_[input_node],
		 correlator_rank, &pending_requests[currreq++]);
    }

    if (control_parameters.cross_polarize()) {
      // duplicate all inputs:
      for (int input_node = 0; input_node < n_inputs; input_node++) {
	int input_rank = input_node + 3;
	connect_to(input_rank,
		   correlator_nr + n_corr_nodes,
		   correlator_rank,
		   input_node + n_inputs,
		   input_node_cnx_params_[input_node],
		   correlator_rank, &pending_requests[currreq++]);
      }
    }

    // Set up the connection to the output node:
    connect_writer_to(correlator_rank, 0,
		      RANK_OUTPUT_NODE, correlator_nr,
		      output_node_cnx_params_[0],
		      correlator_rank, &pending_requests[currreq++]);
  }

  // We simply sum all of the number of connexion eshtablished
  std::vector<MPI_Status> pending_status;
  pending_status.resize(currreq);

  MPI_Waitall(currreq, &pending_requests[0], &pending_status[0]);
  std::cout << "All the connexion are established!" << std::endl;
}

Manager_node::~Manager_node() {
  for (int rank=0; rank < numtasks; rank++) {
    if ((rank != RANK_MANAGER_NODE) &&
        (rank != RANK_LOG_NODE)) {
      end_node(rank);
    }
  }
}

void Manager_node::start() {
  get_log_writer()(1) << "Manager_node::start()" << std::endl;

  PROGRESS_MSG("start correlating");
  initialise();
  current_correlator_node = 0;
  status = START_NEW_SCAN;
  while (status != END_NODE) {
    process_all_waiting_messages();

    switch (status) {
      case START_NEW_SCAN: {
        // set track information
        initialise_scan(control_parameters.scan(current_scan));

        std::vector<bool> input_in_scan(control_parameters.number_inputs(), false);
        int ninputs_in_scan = 0;
        for (size_t input_node = 0; input_node < control_parameters.number_inputs();
             input_node++) {
          for(int ch=0; ch<station_ch_number.size(); ch++){
            if (station_ch_number[ch][input_node] >= 0){
	      input_in_scan[input_node] = true;
              ninputs_in_scan++;
              break;
            }
          }
        }
        // If no inputs participate in the current scan move on to the next
        if (ninputs_in_scan == 0) {
          Time dt = stop_time_scan - start_time;
          // NB: In GOTO_NEXT_TIMESLICE integration_nr will be increased by one
          integration_nr = (int)(dt / integration_time()) - 1;
          if ((dt % integration_time()) != Time()) {
            integration_nr++;
            slice_nr = 0;
          }
          status = GOTO_NEXT_TIMESLICE;
          break;
        }

        // Set the input nodes to the proper start time
        for (size_t input_node = 0; input_node < control_parameters.number_inputs();
             input_node++) {
          if (input_in_scan[input_node]) {
            Time station_time =
              input_node_get_current_time(input_node);
            if (station_time >
                start_time + integration_time() * integration_nr) {
              integration_nr = (int)((station_time - start_time) / integration_time());
              slice_nr = 0;
            }
          }
        }

        // Check whether the new start time is before the stop time
        get_log_writer() << "START_TIME: " << start_time + integration_time() * integration_nr << std::endl;
        if (stop_time <= start_time + integration_time() * integration_nr) {
          status = STOP_CORRELATING;
          break;
        }

        for (size_t input_node = 0; input_node < control_parameters.number_inputs();
             input_node++) {
          if (input_in_scan[input_node]) {
	    const std::string& scan_name =
	      control_parameters.scan(current_scan);
	    const std::string& station_name =
	      control_parameters.station(station_map[input_node]);
	    Time stop_time_station =
	      control_parameters.stop_time(scan_name, station_name);
            input_node_set_time(input_node,
                                start_time + integration_time() * integration_nr,
                                stop_time_scan, stop_time_station);
	  }
        }
        status = START_CORRELATION_TIME_SLICE;
        break;
      }
      case START_CORRELATION_TIME_SLICE: {
        channel_idx = 0;
        status = START_CORRELATOR_NODES_FOR_TIME_SLICE;
        break;
      }
      case START_CORRELATOR_NODES_FOR_TIME_SLICE: {
        bool added_correlator_node = false;
#ifdef SFXC_DETERMINISTIC

        if (correlator_node_ready[current_correlator_node]) {
          set_correlator_node_ready(current_correlator_node, false);
          start_next_timeslice_on_node(current_correlator_node);

          added_correlator_node = true;
        }
#else
        if (!ready_correlator_nodes.empty()) {
          start_next_timeslice_on_node(ready_correlator_nodes.front());
          ready_correlator_nodes.pop();
          added_correlator_node = true;
        }
#endif

        if (added_correlator_node) {
          if (channel_idx == channels_in_scan.size()) {
            status = GOTO_NEXT_TIMESLICE;
          }
        } else {
          // No correlator node added, wait for the next message
          check_and_process_message();
        }

        break;
      }
      case GOTO_NEXT_TIMESLICE: {
	slice_nr++;
	if (slice_nr >= control_parameters.slices_per_integration()) {
	  integration_nr++;
	  slice_nr = 0;
	}
        PROGRESS_MSG("starting timeslice " << start_time + integration_time() * integration_nr);
        // Check whether the integration slice continues past the stop time
        if (start_time + integration_time() * (integration_nr + 1) > stop_time) {
          status = STOP_CORRELATING;

          // Check whether the integration slice continues past the scan
        } else if (start_time + integration_time() * (integration_nr + 1) > stop_time_scan) {
          // We can stop if we finished the last scan
          if (++current_scan == control_parameters.number_scans()) {
            status = STOP_CORRELATING;
          } else {
            status = START_NEW_SCAN;
          }
        } else {
          // Just process the next time slice
          status = START_CORRELATION_TIME_SLICE;
        }
        break;
      }
      case STOP_CORRELATING: {
        // The status is set to END_NODE as soon as the output_node is ready
        MPI_Send(&output_slice_nr, 1, MPI_INT32,
                 RANK_OUTPUT_NODE, MPI_TAG_OUTPUT_NODE_CORRELATION_READY,
                 MPI_COMM_WORLD);

        status = WAIT_FOR_OUTPUT_NODE;
        break;
      }
      case WAIT_FOR_OUTPUT_NODE: {
        // The status is set to END_NODE as soon as the output_node is ready
        check_and_process_message();
        break;
      }
      case END_NODE: {
        break;
      }
    }
  }
  PROGRESS_MSG("terminating nodes");

  get_log_writer()(1) << "Terminating nodes" << std::endl;
}

void Manager_node::terminate()
{
	  PROGRESS_MSG("MESSAGE TERMINATE !");
		status = END_NODE;
}

void Manager_node::start_next_timeslice_on_node(int corr_node_nr) {
  int current_channel = channels_in_scan[channel_idx];
  int cross_channel = -1;
  if (control_parameters.cross_polarize()) {
    cross_channel = control_parameters.cross_channel(current_channel,
                    get_current_mode());
    if ((cross_channel < 0) || (!is_channel_in_scan[cross_channel]))
      cross_channel = -1; 
    SFXC_ASSERT((cross_channel == -1) || (cross_channel > (int)current_channel));
  }

  // Initialise the correlator node
  if (cross_channel == -1) {
    Time time = start_time + integration_time() * integration_nr;
    get_log_writer()(1)
      << "start " << time.date_string()
      << ", slice " << slice_nr
      << ", channel " << current_channel
      << " to correlation node " << corr_node_nr << std::endl;
    PROGRESS_MSG("start " << time.date_string()
		 << ", slice " << slice_nr
                 << ", channel " << current_channel
		 << " to correlation node " << corr_node_nr);
  } else {
    Time time = start_time + integration_time() * integration_nr;
    get_log_writer()(1)
      << "start " << time.date_string()
      << ", slice " << slice_nr
      << ", channel " << current_channel << "," << cross_channel
      << " to correlation node " << corr_node_nr << std::endl;
    PROGRESS_MSG("start " << time.date_string()
		 << ", slice " << slice_nr
                 << ", channel " << current_channel << "," << cross_channel
		 << " to correlation node " << corr_node_nr);
  }

  Correlation_parameters correlation_parameters;
  std::string scan_name = control_parameters.scan(current_scan);
  correlation_parameters =
    control_parameters.
    get_correlation_parameters(scan_name,
                               current_channel,
                               integration_nr,
                               get_input_node_map());
  correlation_parameters.integration_start =
    start_time + integration_time() * integration_nr;
  correlation_parameters.slice_start = correlation_parameters.integration_start +
    correlation_parameters.slice_time * slice_nr;
  if (slice_nr == control_parameters.slices_per_integration() - 1) {
    correlation_parameters.slice_time = 
      correlation_parameters.integration_start +
      correlation_parameters.integration_time -
      correlation_parameters.slice_start;
    int nfft = Control_parameters::nr_correlation_ffts_per_integration(correlation_parameters.slice_time,
								       correlation_parameters.sample_rate,
								       correlation_parameters.fft_size_correlation);
    correlation_parameters.slice_size = correlation_parameters.fft_size_correlation * nfft;
  }
  // stream_start <= slice_start ; needed for coherent dedispersion (place holder for now)
  correlation_parameters.stream_start = correlation_parameters.slice_start;
  correlation_parameters.integration_nr = integration_nr;
  correlation_parameters.slice_nr = output_slice_nr;
  strncpy(correlation_parameters.source, control_parameters.scan_source(scan_name).c_str(), 11);
  correlation_parameters.pulsar_binning = control_parameters.pulsar_binning();
  if (control_parameters.multi_phase_center())
    correlation_parameters.n_phase_centers = n_sources_in_current_scan;
  else
    correlation_parameters.n_phase_centers = 1;
  correlation_parameters.multi_phase_center =
    control_parameters.multi_phase_center();

  correlator_node_set(correlation_parameters, corr_node_nr);

  // set the input streams
  for (size_t input_node = 0; input_node < control_parameters.number_inputs();
       input_node++) {
    int stream = corr_node_nr;
    int stream_idx;

    stream_idx = 0;
    while ((stream_idx < correlation_parameters.station_streams.size()) &&
	   (correlation_parameters.station_streams[stream_idx].station_stream != input_node))
      stream_idx++;
    if (stream_idx == correlation_parameters.station_streams.size())
      continue;

    int64_t slice_samples = correlation_parameters.slice_size *
      correlation_parameters.station_streams[stream_idx].sample_rate /
      correlation_parameters.sample_rate;;

    if (station_ch_number[current_channel][input_node] >= 0) {
      input_node_set_time_slice(input_node,
                                station_ch_number[current_channel][input_node],
                                stream,
                                correlation_parameters.slice_start,
                                correlation_parameters.slice_start +
                                correlation_parameters.slice_time,
                                slice_samples);
      stream += n_corr_nodes;
    }

    if (cross_channel != -1 &&
	station_ch_number[cross_channel][input_node] >= 0) {
      input_node_set_time_slice(input_node,
                                station_ch_number[cross_channel][input_node],
                                stream,
                                correlation_parameters.slice_start,
                                correlation_parameters.slice_start +
                                correlation_parameters.slice_time,
                                slice_samples);
    }
  }

  // Move to the next channel
  channel_idx +=1;
  while ((control_parameters.cross_polarize()) && (channel_idx < channels_in_scan.size())) {
    current_channel = channels_in_scan[channel_idx];
    cross_channel = control_parameters.cross_channel(current_channel,
                                                       get_current_mode());
    if ((cross_channel == -1) || (cross_channel > (int)current_channel)) {
      break;
    }
    channel_idx += 1;
  }
#ifdef SFXC_DETERMINISTIC
  current_correlator_node = (current_correlator_node+1)%correlator_node_ready.size();
#endif
  output_slice_nr++;
}

void
Manager_node::initialise() {
  get_log_writer()(1) << "Initialising the Input_nodes" << std::endl;
  for (size_t input_node = 0; input_node < control_parameters.number_inputs();
       input_node++) {
    // setting the first data-source of the first station
    const std::string &station = control_parameters.station(station_map[input_node]);
    const std::string &datastream = datastream_map[input_node];
    set_data_reader(input_node + 3, 0,
		    control_parameters.data_sources(station, datastream));
  }

  start_time = control_parameters.get_start_time();
  stop_time = control_parameters.get_stop_time();

  // Find first scan
  current_scan = control_parameters.scan(start_time.date_string());
  if (current_scan == -1) {
    std::cerr << "Cannot find scan corresponding to start time "
	      << start_time.date_string() << std::endl;
    sfxc_abort();
  }
  SFXC_ASSERT(current_scan < control_parameters.number_scans());

  // build a list of all source in the current job
  {
  const Vex vex = control_parameters.get_vex();

  Vex::Date start_time(control_parameters.get_start_time().date_string());
  Vex::Date stop_time(control_parameters.get_stop_time().date_string());
  std::string first_scan_name = vex.get_scan_name(start_time);
  Vex::Node::const_iterator it = vex.get_root_node()["SCHED"][first_scan_name];
  std::cout << "Getting sources starting scan " << first_scan_name << " ; " << it.key()
	    << ", stop_time = " << stop_time.to_string()
	    << ", tstart = " << vex.start_of_scan(it.key()).to_string() << "\n";
  while(it != vex.get_root_node()["SCHED"]->end() && vex.start_of_scan(it.key()) < stop_time){
    Vex::Node::const_iterator sources_it = it->begin("source");
    while(sources_it != it->end("source")){
      sources.insert(sources_it->to_string());
      std::cout << "found source " << sources_it->to_string() << " in scan " << it.key() << std::endl;
      sources_it++;
    }
    it++;
  }
  }
  correlator_node_set_all(sources);
  

  if (control_parameters.get_mask_parameters(mask_parameters))
    correlator_node_set_all(mask_parameters);

  if(control_parameters.pulsar_binning()){
    // If pulsar binning is enabled : get all pulsar parameters (polyco files, etc.)
    if (!control_parameters.get_pulsar_parameters(pulsar_parameters))
      sfxc_abort("Error parsing pulsar information from control file\n");
    correlator_node_set_all(pulsar_parameters);
    // Set the output files (minimum of two bins for off-pulse data)
    int max_nbins=2;
    std::map<std::string, Pulsar_parameters::Pulsar>::iterator it;
    for ( it=pulsar_parameters.pulsars.begin() ; it != pulsar_parameters.pulsars.end(); it++ ){
      max_nbins = std::max(it->second.nbins + 1, max_nbins);
    }
    std::string base_filename = control_parameters.get_output_file();
    // Open one output file per pulsar bin
    for(int bin=0;bin<max_nbins;bin++){
      std::ostringstream outfile;
      outfile << base_filename << ".bin" << bin;
      set_data_writer(RANK_OUTPUT_NODE, bin, outfile.str());
    }
  }else if(control_parameters.multi_phase_center()){
    SFXC_ASSERT(!control_parameters.pulsar_binning());

    // open one output file per source
    std::string base_filename = control_parameters.get_output_file();
    std::set<std::string>::iterator sources_it = sources.begin();
    int source_nr=0;
    while(sources_it != sources.end()){
      set_data_writer(RANK_OUTPUT_NODE, source_nr, base_filename + "_" + *sources_it);
      sources_it++;
      source_nr++;
    }
  }else
    set_data_writer(RANK_OUTPUT_NODE, 0, control_parameters.get_output_file());

  {
    std::string filename = control_parameters.get_phasecal_file();
    if (!filename.empty()) {
      SFXC_ASSERT(strncmp(filename.c_str(), "file://", 7) == 0);
      int len = filename.size() + 1;
      char msg[len];
      strncpy(msg, filename.c_str(), len);
      MPI_Send(msg, len, MPI_CHAR, RANK_OUTPUT_NODE, MPI_TAG_OUTPUT_NODE_SET_PHASECAL_FILE, MPI_COMM_WORLD);
    }
  }

  {
    std::string filename = control_parameters.get_tsys_file();
    if (!filename.empty()) {
      SFXC_ASSERT(strncmp(filename.c_str(), "file://", 7) == 0);
      int len = filename.size() + 1;
      char msg[len];
      strncpy(msg, filename.c_str(), len);
      MPI_Send(msg, len, MPI_CHAR, RANK_OUTPUT_NODE, MPI_TAG_OUTPUT_NODE_SET_TSYS_FILE, MPI_COMM_WORLD);
    }
  }

  // Write the global header in the outpul file
  send_global_header();

  output_slice_nr = 0;

  PROGRESS_MSG("start_time: " << start_time.date_string());
  PROGRESS_MSG("stop_time: " << stop_time.date_string());

  get_log_writer()(1) << "Starting correlation" << std::endl;
}

void Manager_node::initialise_scan(const std::string &scan) {
  const Vex &vex = control_parameters.get_vex();
  Vex::Date start_of_scan = vex.start_of_scan(scan);
  Vex::Date stop_of_scan = vex.stop_of_scan(scan);

  int start_mjd = mjd(1, 1, start_of_scan.year) + start_of_scan.day - 1;
  Time scan_start(start_mjd, start_of_scan.to_miliseconds() / 1000.);

  // set the start time to the beginning of the scan
  if (start_time + integration_time() * integration_nr < scan_start) {
    Time start_interval = scan_start - start_time;
    integration_nr = (int)(start_interval / integration_time());
    if ((start_interval % integration_time()) != Time()) {
      integration_nr++;
      slice_nr = 0;
    }
  }

  int stop_mjd = mjd(1, 1, stop_of_scan.year) + stop_of_scan.day - 1;
  stop_time_scan = Time(stop_mjd, stop_of_scan.to_miliseconds() / 1000.);

  if (stop_time < stop_time_scan)
    stop_time_scan = stop_time;
  // Align the stop time with the time slices
  SFXC_ASSERT(((stop_time_scan-start_time)%integration_time()) >= Time());
  stop_time_scan -= (stop_time_scan-start_time)%integration_time();
  SFXC_ASSERT(((stop_time_scan-start_time)%integration_time()) == Time());

  // Send the delay tables:
  get_log_writer() << "Set delay_table" << std::endl;
  for (size_t input_node = 0; input_node < control_parameters.number_inputs();
       input_node++) {
    int input_rank = input_node + 3;
    const std::string &station_name = control_parameters.station(station_map[input_node]);
    if (!control_parameters.station_in_scan(scan, station_name))
      continue;
    Delay_table delay_table;
    const std::string &delay_file =
      control_parameters.get_delay_table_name(station_name); // also generates delay file if it doesn't exist
    delay_table.open(delay_file.c_str(), scan_start, stop_time_scan, scan);
    SFXC_ASSERT_MSG(delay_table.initialised(), 
                    ("can't load delay table " + delay_file).c_str());

    // Get clock offset
    const Vex::Node &root = vex.get_root_node();
    Vex::Node::const_iterator clock = root["STATION"][station_name]["CLOCK"];
    if (clock == root["STATION"][station_name]->end()) {
      std::cerr << "Cannot find $CLOCK reference for " << station_name << std::endl;
      sfxc_abort();
    }
    const std::string &clock_name = clock->to_string();
    if (root["CLOCK"][clock_name] == root["CLOCK"]->end()) {
      std::cerr << "Cannot find " << clock_name << " in $CLOCK block" << std::endl;
      sfxc_abort();
    }
    clock = root["CLOCK"][clock_name]["clock_early"];
    if (clock == root["CLOCK"][clock_name]->end()) {
      std::cerr << "Cannot find clock for " << station_name << std::endl;
      sfxc_abort();
    }
    Time start, epoch;
    double offset = 0.0, rate = 0.0;
    for (clock = root["CLOCK"][clock_name]->begin("clock_early");
	 clock != root["CLOCK"][clock_name]->end("clock_early"); clock++) {
      if (scan_start < Time(clock[0]->to_string()))
	continue;
      if (start > Time(clock[0]->to_string()))
	continue;
      start = Time(clock[0]->to_string());
      offset = clock[1]->to_double_amount("sec");
      rate = 0.0;
      if (clock->size() > 3) {
	if (clock[3]->to_string().find_first_of("\t ") != std::string::npos) {
	  rate = clock[3]->to_double_amount("sec/sec");
	} else {
	  // No units were specified.  It would make sense to
	  // interpret this as a proper dimensionless quantity.
	  // Unfortunately historical practice at JIVE forces us to
	  // assume rates are specified in usec/sec.
	  rate = clock[3]->to_double() * 1e-6;
	}
	epoch = Time(clock[2]->to_string());
      }
    }
    if (start == Time()) {
      std::cerr << "Clock doesn't cover scan " << scan
		<< " for station " << station_name << std::endl;
      sfxc_abort();
    }

    // To allow large clock offsets, the reader time is adjusted
    const double max_offset = 1.0;
    double reader_offset = round(offset / max_offset) * max_offset;
    offset = offset - reader_offset;
#if 0
    std::cout.precision(19);
    std::cout << "offset = " << offset << ", reader_offset = " << reader_offset << std::endl;
#endif
    delay_table.set_clock_offset(offset, start, rate, epoch);
    send(delay_table, /* station_nr */ 0, input_rank);
    control_parameters.set_reader_offset(station_name, Time(reader_offset*1e6));
    correlator_node_set_all(delay_table, input_node);
  }

  // Send the UVW tables:
  get_log_writer() << "Set uvw_table" << std::endl;
  for (size_t input_node = 0; input_node < control_parameters.number_inputs();
       input_node++) {
    const std::string &station_name = control_parameters.station(station_map[input_node]);
    if (!control_parameters.station_in_scan(scan, station_name))
      continue;
    Uvw_model uvw_table;
    const std::string &delay_file =
      control_parameters.get_delay_table_name(station_name);
    uvw_table.open(delay_file.c_str(), scan_start, stop_time_scan, scan);

    correlator_node_set_all(uvw_table, input_node);
  }

  get_log_writer() << "Set track parameters" << std::endl;

  // Send the track parameters to the input nodes
  const std::string &mode_name = vex.get_mode(scan);
  for (size_t input_node = 0; input_node < control_parameters.number_inputs();
       input_node++) {
    const std::string &station_name = control_parameters.station(station_map[input_node]);
    if(!control_parameters.station_in_scan(scan, station_name))
      continue;

    const std::string &ds_name = datastream_map[input_node];
    Input_node_parameters input_node_param =
      control_parameters.get_input_node_parameters(mode_name, station_name, ds_name);
    if (!input_node_param.channels.empty())
      input_node_set(input_node, input_node_param);
  }
  n_sources_in_current_scan = control_parameters.get_vex().n_sources(scan);

  // Determine for each station which channels are to be correlated
  std::vector<int> last_channel(control_parameters.number_inputs(), -1);
  std::set<int> channels_found;
  station_ch_number.resize(control_parameters.number_frequency_channels());
  for(int i = 0; i < station_ch_number.size(); i++){
    station_ch_number[i].resize(control_parameters.number_inputs());
    for(int input_node = 0; input_node < station_ch_number[i].size(); input_node++){
      std::string station_name = control_parameters.station(station_map[input_node]);
      station_ch_number[i][input_node] = -1;
      if (control_parameters.station_in_scan(scan, station_name)){
        std::string channel_name = control_parameters.frequency_channel(i, mode_name, station_name);
	std::string datastream_name = control_parameters.datastream(mode_name, station_name, channel_name);
        if (!channel_name.empty() && datastream_name == datastream_map[input_node]) {
          channels_found.insert(i);
          station_ch_number[i][input_node] = last_channel[input_node] + 1;
          last_channel[input_node] += 1;
        }
      }
    }
  }
  
  // Create list of all channels to be correlated in the current scan
  is_channel_in_scan.resize(control_parameters.number_frequency_channels());
  is_channel_in_scan.assign(is_channel_in_scan.size(), false);
  channels_in_scan.resize(0);
  for (std::set<int>::iterator it = channels_found.begin(); 
       it != channels_found.end();
       ++it) {
    channels_in_scan.push_back(*it);
    is_channel_in_scan[*it] = true;
  }
}

void Manager_node::end_correlation() {
  SFXC_ASSERT(status == WAIT_FOR_OUTPUT_NODE);
  status = END_NODE;
}

std::string Manager_node::get_current_mode() const {
  std::string scan_name = control_parameters.scan(current_scan);
  return control_parameters.get_vex().get_mode(scan_name);
}

void Manager_node::send_global_header(){ 
  size_t len = sizeof(Output_header_global);;
  std::set<std::string>::iterator it;

  std::set<std::string> stations;
  const Vex vex = control_parameters.get_vex();
  for (Vex::Node::const_iterator it = vex.get_root_node()["STATION"]->begin();
       it != vex.get_root_node()["STATION"]->end(); it++) {
    stations.insert(it.key());
  }

  for (it = stations.begin(); it != stations.end(); it++)
    len += it->size() + 1;
  for (it = sources.begin(); it != sources.end(); it++)
    len += it->size() + 1;

  // Send the global header
  Output_header_global *output_header;
  output_header = (Output_header_global *)malloc(len);
  SFXC_ASSERT(output_header != NULL);
  memset(output_header, 0, len);
  output_header->header_size = len;
  strcpy(output_header->experiment,           // Name of the experiment
      control_parameters.get_exper_name().c_str());
  Time start = control_parameters.get_start_time();
  int start_year, start_day;
  // Start year and day (day of year) of the experiment
  start.get_date(start_year, start_day);
  output_header->start_year = start_year;     // Start year of the experiment
  output_header->start_day = start_day;       // Start day of the experiment (day of year)
  // Start time of the correlation in seconds since midnight
  output_header->start_time = (int)start.get_time();
  output_header->number_channels = control_parameters.number_channels();  // Number of frequency channels
  Time int_time = control_parameters.integration_time();// Integration time: microseconds
  output_header->integration_time = (int)int_time.get_time_usec();
  output_header->output_format_version = OUTPUT_FORMAT_VERSION;
    
  const char *svn_version = SVN_VERSION;
  if (strchr(svn_version, ':'))
    svn_version = strchr(svn_version, ':') + 1;
  output_header->correlator_version = atoi(svn_version);

  output_header->polarisation_type =
    control_parameters.polarisation_type_for_global_output_header(get_current_mode());
  strncpy(output_header->correlator_branch, SVN_BRANCH, 15);
  output_header->correlator_branch[14] = 0;
  output_header->job_nr = control_parameters.job_nr();
  output_header->subjob_nr = control_parameters.subjob_nr();

  char *data = (char *)(output_header + 1);
  output_header->stations_offset = data - (char *)output_header;
  output_header->number_stations = stations.size();
  for (it = stations.begin(); it != stations.end(); it++) {
    strcpy(data, it->c_str());
    data += it->size() + 1;
  }
  output_header->sources_offset = data - (char *)output_header;
  output_header->number_sources = sources.size();
  for (it = sources.begin(); it != sources.end(); it++) {
    strcpy(data, it->c_str());
    data += it->size() + 1;
  }

  output_node_set_global_header((char *)output_header, len);
}

void Manager_node::get_state(std::ostream &out) {
  out << "{\n"
      << "\t\"rank\": " << RANK_OF_NODE << ",\n"
      << "\t\"host\": \"" << HOSTNAME_OF_NODE << "\",\n"
      << "\t\"id\": \"" << ID_OF_NODE << "\",\n"
      << "\t\"now\": \"" << Time::now() << "\",\n"
      << "\t\"state\": ";
  switch(status) {
    case START_NEW_SCAN:
      out << "\"START_NEW_SCAN\",\n";
      break;
    case START_CORRELATION_TIME_SLICE:
      out << "\"START_CORRELATION_TIME_SLICE\",\n";
      break;
    case START_CORRELATOR_NODES_FOR_TIME_SLICE:
      out << "\"START_CORRELATOR_NODES_FOR_TIME_SLICE\",\n";
      break;
    case GOTO_NEXT_TIMESLICE:
      out << "\"GOTO_NEXT_TIMESLICE\",\n";
      break;
    case STOP_CORRELATING:
      out << "\"STOP_CORRELATING\",\n";
      break;
    case WAIT_FOR_OUTPUT_NODE:
      out << "\"WAIT_FOR_OUTPUT_NODE\",\n";
      break;
    case END_NODE:
      out << "\"END_NODE\",\n";
      break;
    default:
      out << "\"UNKNOWN_STATE\",\n";
  };
#ifdef SFXC_DETERMINISTIC
  int nfree = 0;
  for (int i = 0; i < correlator_node_ready.size(); i++) {
    if (correlator_node_ready[i])
      nfree++;
  }
#else
  int nfree = ready_correlator_nodes.size();
#endif
  out << "\t\"current_time\": \"" << start_time + integration_time() * integration_nr << "\",\n"
      << "\t\"integration_nr\": " << integration_nr << ",\n"
      << "\t\"current_scan\": \"" << control_parameters.scan(current_scan) << "\",\n"
      << "\t\"current_channel\": " << channels_in_scan[channel_idx] << ",\n"
      << "\t\"number_input_nodes\": " << get_control_parameters().number_inputs() << ",\n"
      << "\t\"number_correlator_nodes\": " << numtasks - (get_control_parameters().number_inputs() + 3) << ",\n"
      << "\t\"number_free_correlator_nodes\": " << nfree << "\n"
      << "}";
}
