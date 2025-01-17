/* Copyright (c) 2007 Joint Institute for VLBI in Europe (Netherlands)
 * All rights reserved.
 *
 * Author(s): Nico Kruithof <Kruithof@JIVE.nl>, 2007
 *
 * $Id: Node.cc 283 2007-07-12 12:13:17Z kruithof $
 *
 */

#include <iostream>

#include "abstract_manager_node.h"
#include "mpi_transfer.h"
#include "utils.h"
#include "exception_common.h"

// Abstract_manager_node::
// Abstract_manager_node(int rank, int numtasks, const Control_parameters &param)
//     : Node(rank), control_parameters(param), numtasks(numtasks) {}
Abstract_manager_node::
Abstract_manager_node(int rank, int numtasks,
                      Log_writer *writer,
                      const Control_parameters &param)
    : Node(rank, writer), control_parameters(param), numtasks(numtasks), pulsar_parameters(*writer) {
  integration_time_ = Time(param.integration_time());
  }

Abstract_manager_node::~Abstract_manager_node() {}

// Start nodes:
void
Abstract_manager_node::
start_input_node(int rank, const std::string &station, const std::string &datastream) {
  input_node_map[stream_key(station, datastream)] = input_node_rank.size();
  input_node_rank.push_back(rank);

  // Get the mode of the first scan
  const Vex vex = control_parameters.get_vex();
  Time start_time = control_parameters.get_start_time();
  int current_scan = control_parameters.scan(start_time.date_string());
  if (current_scan == -1) {
    std::cerr << "Cannot find scan corresponding to start time "
	      << start_time.date_string() << std::endl;
    sfxc_abort();
  }
  std::string scan_name = control_parameters.scan(current_scan);
  std::string mode = control_parameters.get_vex().get_mode(scan_name); 

  // Start the appropriate input reader.
  int station_number = control_parameters.station_number(station);
  if (control_parameters.data_format(station, mode) == "Mark4") {
    MPI_Send(&station_number, 1, MPI_INT32,
	     rank, MPI_TAG_SET_INPUT_NODE_MARK5A, MPI_COMM_WORLD);
  } else if (control_parameters.data_format(station, mode) == "VLBA") {
    MPI_Send(&station_number, 1, MPI_INT32,
	     rank, MPI_TAG_SET_INPUT_NODE_VLBA, MPI_COMM_WORLD);
  } else if (control_parameters.data_format(station, mode) == "VDIF") {
    MPI_Send(&station_number, 1, MPI_INT32,
	     rank, MPI_TAG_SET_INPUT_NODE_VDIF, MPI_COMM_WORLD);
  } else {
    if (control_parameters.data_format(station, mode) != "Mark5B")
      std::cerr << "Invalid DAS for station " << station << "\n";
    SFXC_ASSERT(control_parameters.data_format(station, mode) == "Mark5B");
    MPI_Send(&station_number, 1, MPI_INT32,
             rank, MPI_TAG_SET_INPUT_NODE_MARK5B, MPI_COMM_WORLD);
  }
  Time ref_time(control_parameters.get_vex().get_start_time_of_experiment());
  MPI_Send(&ref_time,  1, MPI_INT64, rank, MPI_TAG_SET_INPUT_NODE_REF_DATE,  MPI_COMM_WORLD);
  MPI_Send((void *)station.c_str(),  station.size()+1, MPI_CHAR, rank, 
           MPI_TAG_SET_INPUT_SET_STATION_NAME, MPI_COMM_WORLD);
  ///DEBUG_MSG("WAITING FOR NODE PARAMTERS !");
  /// add a new set of parameters
  Connexion_params* params= new Connexion_params();
  input_node_cnx_params_.push_back(params);

  /// receive the connexion parameters for the current input_node
  MPI_Transfer::receive_ip_address(params->ip_port_, params->hostname_, rank);

  /// wait to receive the the acknowledment showing that hte node
  /// is correctly initialized.
  MPI_Status status;
  int msg;
  MPI_Recv(&msg, 1, MPI_INT32,
           rank, MPI_TAG_NODE_INITIALISED, MPI_COMM_WORLD,
           &status);

  set_message_level(rank, get_log_writer().get_maxlevel());
}
void
Abstract_manager_node::
start_output_node(int rank) {
  SFXC_ASSERT(rank == RANK_OUTPUT_NODE);
  // starting an input reader
  int32_t msg=0;
  MPI_Send(&msg, 1, MPI_INT32,
           rank, MPI_TAG_SET_OUTPUT_NODE, MPI_COMM_WORLD);


  ///DEBUG_MSG("WAITING FOR OUTPUT_NODE PARAMETERS !");
  /// add a new set of parameters
  Connexion_params* params= new Connexion_params();
  output_node_cnx_params_.push_back(params);

  /// receive the connexion parameters for the current input_node
  MPI_Transfer::receive_ip_address(params->ip_port_, params->hostname_, rank);

  MPI_Status status;
  MPI_Recv(&msg, 1, MPI_INT32,
           rank, MPI_TAG_NODE_INITIALISED, MPI_COMM_WORLD,
           &status);

  set_message_level(rank, get_log_writer().get_maxlevel());
}
void
Abstract_manager_node::
start_correlator_node(int rank) {
  size_t correlator_node_nr = correlator_node_rank.size();
#ifdef SFXC_DETERMINISTIC

  correlator_node_ready.resize(correlator_node_nr+1);
  SFXC_ASSERT(correlator_node_nr < correlator_node_ready.size());
  set_correlator_node_ready(correlator_node_nr, false);
#endif

  correlator_node_rank.push_back(rank);

  // starting a correlator node
  if(control_parameters.pulsar_binning())
    MPI_Send(&correlator_node_nr, 1, MPI_INT32, rank,
             MPI_TAG_SET_CORRELATOR_NODE_PSR_BINNING, MPI_COMM_WORLD);
  else if(control_parameters.phased_array())
    MPI_Send(&correlator_node_nr, 1, MPI_INT32, rank,
             MPI_TAG_SET_CORRELATOR_NODE_PHASED, MPI_COMM_WORLD);
  else
    MPI_Send(&correlator_node_nr, 1, MPI_INT32, rank,
             MPI_TAG_SET_CORRELATOR_NODE, MPI_COMM_WORLD);

  int msg;
  MPI_Status status;
  MPI_Recv(&msg, 1, MPI_INT32,
           rank, MPI_TAG_NODE_INITIALISED,
           MPI_COMM_WORLD, &status);

  set_message_level(rank, get_log_writer().get_maxlevel());
}

void
Abstract_manager_node::
start_log_node(int rank) {
  SFXC_ASSERT(rank == RANK_LOG_NODE);
  int msg=0;
  // Log node:
  MPI_Send(&msg, 1, MPI_INT32,
           RANK_LOG_NODE, MPI_TAG_SET_LOG_NODE, MPI_COMM_WORLD);
  MPI_Send(&msg, 1, MPI_INT32,
           RANK_LOG_NODE, MPI_TAG_LOG_NODE_SET_OUTPUT_COUT, MPI_COMM_WORLD);

  MPI_Status status;
  MPI_Recv(&msg, 1, MPI_INT32,
           RANK_LOG_NODE, MPI_TAG_NODE_INITIALISED, MPI_COMM_WORLD, &status);
  set_message_level(rank, get_log_writer().get_maxlevel());
}

void
Abstract_manager_node::
start_log_node(int rank, char *filename) {
  SFXC_ASSERT(rank == RANK_LOG_NODE);
  int msg=0;
  // Log node:
  MPI_Send(&msg, 1, MPI_INT32,
           RANK_LOG_NODE, MPI_TAG_SET_LOG_NODE, MPI_COMM_WORLD);
  MPI_Send(filename, strlen(filename)+1, MPI_CHAR,
           RANK_LOG_NODE, MPI_TAG_LOG_NODE_SET_OUTPUT_FILE, MPI_COMM_WORLD);

  MPI_Status status;
  MPI_Recv(&msg, 1, MPI_INT32,
           RANK_LOG_NODE, MPI_TAG_NODE_INITIALISED, MPI_COMM_WORLD, &status);

  set_message_level(rank, get_log_writer().get_maxlevel());
}

void
Abstract_manager_node::set_message_level(int rank, int32_t messagelevel) {
  MPI_Send(&messagelevel, 1, MPI_INT32,
           rank, MPI_TAG_SET_MESSAGELEVEL, MPI_COMM_WORLD);
}

void
Abstract_manager_node::
end_node(int rank) {
  int32_t type = 0;
  MPI_Send(&type, 1, MPI_INT32,
           rank, MPI_TAG_END_NODE, MPI_COMM_WORLD);
}

int
Abstract_manager_node::
get_status(int rank) {
  int32_t result = 0;
  MPI_Send(&result, 1, MPI_INT32,
           rank, MPI_TAG_GET_STATUS, MPI_COMM_WORLD);

  MPI_Status status;
  MPI_Recv(&result, 1, MPI_INT32,
           rank, MPI_TAG_GET_STATUS, MPI_COMM_WORLD, &status);
  return result;
}

/// Setting of the data readers/writers
void
Abstract_manager_node::
set_data_reader(int rank, int32_t stream_nr,
                const std::vector<std::string> &sources) {
  int len = sizeof(int32_t);

  for (int i = 0; i < sources.size(); i++)
    len += sources[i].size() + 1;

  char msg[len];
  char *p = msg;
  memcpy(p, &stream_nr, sizeof(int32_t));
  p += sizeof(int32_t);

  for (int i = 0; i < sources.size(); i++) {
    memcpy(p, sources[i].c_str(), sources[i].size() + 1);
    p += sources[i].size() + 1;
  }

  MPI_Send(msg, len, MPI_CHAR,
           rank, MPI_TAG_ADD_DATA_READER, MPI_COMM_WORLD);

  wait_for_setting_up_channel(rank);
}

void Abstract_manager_node::
set_data_writer_void(int writer_rank, int writer_stream_nr) {
  //DEBUG_MSG(writer_rank<< "[" << writer_stream_nr << "] => void");
  MPI_Send(&writer_stream_nr, 1, MPI_INT32,
           writer_rank, MPI_TAG_ADD_DATA_WRITER_VOID2, MPI_COMM_WORLD);
  wait_for_setting_up_channel(writer_rank);
}

void
Abstract_manager_node::
set_data_writer(int rank, int stream_nr,
                const std::string &filename) {
  //DEBUG_MSG(rank << "[" << stream_nr << "] => " << filename);
  SFXC_ASSERT(strncmp(filename.c_str(), "file://", 7) == 0);
  int len = sizeof(int32_t) + filename.size() +1; // for \0
  char msg[len];
  memcpy(msg,&stream_nr,sizeof(int32_t));
  memcpy(msg+sizeof(int32_t), filename.c_str(), filename.size()+1);
  SFXC_ASSERT(msg[len-1] == '\0');

  MPI_Send(msg, len, MPI_CHAR,
           rank, MPI_TAG_ADD_DATA_WRITER_FILE2, MPI_COMM_WORLD);

  wait_for_setting_up_channel(rank);
}

void
Abstract_manager_node::set_TCP(int writer_rank, int writer_stream_nr,
                               int reader_rank, int reader_stream) {
  // DEBUG_MSG(writer_rank << "[" << writer_stream_nr << "] => "
  //          << reader_rank << "[" << reader_stream << "]");
  int32_t msg[3] = {writer_stream_nr, reader_rank, reader_stream};
  MPI_Send(msg, 3, MPI_INT32,
           writer_rank, MPI_TAG_ADD_TCP, MPI_COMM_WORLD);

  wait_for_setting_up_channel(writer_rank);
}

void
Abstract_manager_node::connect_to(
  int writer_rank,
  int writer_stream_nr,
  int reader_rank,
  int reader_stream_nr,
  Connexion_params* params, int rank, MPI_Request* req) {
  // DEBUG_MSG(writer_rank << "[" << writer_stream_nr << "] => "
  //          << reader_rank << "[" << reader_stream << "]");
  uint32_t msg[4] = {writer_rank, writer_stream_nr, reader_rank, reader_stream_nr};

  // connect to some tcp endpoint
  MPI_Transfer::send_connect_to_msg(msg, params->ip_port_, params->hostname_, rank);

  // req is used to receive the acknowledgment
  CHECK_MPI( MPI_Irecv( NULL, 0, MPI_UINT32,
                        rank, MPI_TAG_CONNECTION_ESTABLISHED,
                        MPI_COMM_WORLD,
                        req ) );
}

void
Abstract_manager_node::connect_writer_to(
  int writer_rank,
  int writer_stream_nr,
  int reader_rank,
  int reader_stream_nr,
  Connexion_params* params, int rank, MPI_Request* req) {
  // DEBUG_MSG(writer_rank << "[" << writer_stream_nr << "] => "
  //          << reader_rank << "[" << reader_stream << "]");
  uint32_t msg[4] = {writer_rank, writer_stream_nr, reader_rank, reader_stream_nr};

  // connect to some tcp endpoint
  MPI_Transfer::send_connect_writer_to_msg(msg, params->ip_port_, params->hostname_, rank);

  // req is used to receive the acknowledgment
  CHECK_MPI( MPI_Irecv( NULL, 0, MPI_UINT32,
                        rank, MPI_TAG_CONNECTION_ESTABLISHED,
                        MPI_COMM_WORLD,
                        req ) );
}

void
Abstract_manager_node::
input_node_set(int input_node, Input_node_parameters &input_node_params) {
  int rank = input_node + 3;
  MPI_Transfer::send(input_node_params, rank);
}

Time
Abstract_manager_node::
input_node_get_current_time(int input_node) {
  int rank = input_node + 3;
  int64_t nticks;
  MPI_Send(&nticks, 1, MPI_INT64,
           rank, MPI_TAG_INPUT_NODE_GET_CURRENT_TIMESTAMP, MPI_COMM_WORLD);
  MPI_Status status;
  MPI_Recv(&nticks, 1, MPI_INT64, rank,
           MPI_TAG_INPUT_NODE_GET_CURRENT_TIMESTAMP, MPI_COMM_WORLD, &status);
  Time result;
  result.set_clock_ticks(nticks);
  return result;
}

void
Abstract_manager_node::
input_node_set_time(int input_node,
                    Time start_time, Time stop_time, Time leave_time) {
  int rank = input_node + 3;
  SFXC_ASSERT(start_time < stop_time);
  SFXC_ASSERT(start_time < leave_time);
  int64_t time[3];
  time[0] = start_time.get_clock_ticks();
  time[1] = stop_time.get_clock_ticks();
  time[2] = leave_time.get_clock_ticks();
  MPI_Send(&time[0], 3, MPI_INT64,
           rank, MPI_TAG_INPUT_NODE_SET_TIME, MPI_COMM_WORLD);
}

void
Abstract_manager_node::
input_node_set_time_slice(int input_node,
                          int32_t channel, int32_t stream_nr,
                          Time start_time, Time stop_time,
                          int64_t slice_samples) {
  int rank = input_node + 3;
  int64_t message[] = {channel,
                       stream_nr,
                       start_time.get_clock_ticks(),
                       stop_time.get_clock_ticks(),
                       slice_samples};
  MPI_Send(&message, 5, MPI_INT64,
           rank, MPI_TAG_INPUT_NODE_ADD_TIME_SLICE, MPI_COMM_WORLD);
}

void
Abstract_manager_node::
wait_for_setting_up_channel(int rank) {
  while (true) {
    MPI_Status status;
    if (rank >= 0) {
      MPI_Probe(rank, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
    } else {
      MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
    }

    // Check whether we have found the message with the right tag
    // and return if we did
    if (status.MPI_TAG == MPI_TAG_CONNECTION_ESTABLISHED) {
      int32_t channel;
      MPI_Status status2;
      MPI_Recv(&channel, 1, MPI_INT32, status.MPI_SOURCE,
               MPI_TAG_CONNECTION_ESTABLISHED, MPI_COMM_WORLD, &status2);
      return;
    }

    // We received another message, process it.
    check_and_process_waiting_message();
  }
}

Node::MESSAGE_RESULT
Abstract_manager_node::check_and_process_waiting_message() {
  MPI_Status status;
  int result;
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &result, &status);
  if (result) {
    if (status.MPI_TAG == MPI_TAG_ASSERTION_RAISED) {
      MPI_Status status2;
      int32_t msg;
      MPI_Recv(&msg, 1, MPI_INT32, status.MPI_SOURCE,
               status.MPI_TAG, MPI_COMM_WORLD, &status2);
      terminate_nodes_after_assertion(status.MPI_SOURCE);
      terminate();
      return MESSAGE_PROCESSED;
    } else {
      return Node::check_and_process_waiting_message();
    }
  }
  return NO_MESSAGE;
}

Node::MESSAGE_RESULT
Abstract_manager_node::process_all_waiting_messages() {
  MESSAGE_RESULT result;
  do {
    result = check_and_process_waiting_message();
  } while (result == MESSAGE_PROCESSED);
  return result;
}

Node::MESSAGE_RESULT
Abstract_manager_node::check_and_process_message() {
  MPI_Status status;
  MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

  if (status.MPI_TAG == MPI_TAG_ASSERTION_RAISED) {
    MPI_Status status2;
    int32_t msg;
    MPI_Recv(&msg, 1, MPI_INT32, status.MPI_SOURCE,
             status.MPI_TAG, MPI_COMM_WORLD, &status2);
    terminate_nodes_after_assertion(status.MPI_SOURCE);
    terminate();
    return MESSAGE_PROCESSED;
  } else {
    return Node::check_and_process_message();
  }
}



const Control_parameters &
Abstract_manager_node::
get_control_parameters() const {
  return control_parameters;
}

int
Abstract_manager_node::
get_number_of_processes() const {
  return numtasks;
}

size_t
Abstract_manager_node::
number_correlator_nodes() const {
  return correlator_node_rank.size();
}

void
Abstract_manager_node::
correlator_node_set(Correlation_parameters &parameters,
                    int corr_node_nr) {
  MPI_Transfer::send(parameters,correlator_node_rank[corr_node_nr]);
}

void
Abstract_manager_node::
correlator_node_set_all(Delay_table &delay_table, int input_node) {
  int sn[2] = {input_node, -1};
  if (control_parameters.cross_polarize())
    sn[1] = sn[0] + control_parameters.number_inputs();

  MPI_Transfer::bcast_corr_nodes(delay_table, sn);
}

void
Abstract_manager_node::
correlator_node_set_all(Uvw_model &uvw_table, int input_node) {
  MPI_Transfer::bcast_corr_nodes(uvw_table, input_node);
}

void
Abstract_manager_node::
correlator_node_set_all(Pulsar_parameters &pulsar) {
  for (size_t i=0; i<correlator_node_rank.size(); i++) {
    MPI_Transfer::send(pulsar, correlator_node_rank[i]);
  }
}

void
Abstract_manager_node::
correlator_node_set_all(Mask_parameters &mask) {
  MPI_Transfer::bcast_corr_nodes(mask);
}

void
Abstract_manager_node::
correlator_node_set_all(std::set<std::string> &sources) {
  for (size_t i=0; i<correlator_node_rank.size(); i++) {
    MPI_Transfer::send(sources, correlator_node_rank[i]);
  }
}


void
Abstract_manager_node::
set_correlator_node_ready(size_t correlator_nr, bool ready) {
#ifdef SFXC_DETERMINISTIC
  SFXC_ASSERT(correlator_nr < correlator_node_ready.size());
  correlator_node_ready[correlator_nr] = ready;
#else

  if (ready) {
    ready_correlator_nodes.push(correlator_nr);
  }
#endif
}


void
Abstract_manager_node::
send(Delay_table &delay_table, int station, int to_rank) {
  MPI_Transfer::send(delay_table, station, to_rank);

}
const std::map<stream_key, int> &
Abstract_manager_node::get_input_node_map() const {
  return input_node_map;
}

void
Abstract_manager_node::
output_node_set_global_header(char* header_msg, int size) {
  MPI_Send(header_msg, size, MPI_CHAR,
           RANK_OUTPUT_NODE,
           MPI_TAG_OUTPUT_NODE_GLOBAL_HEADER,
           MPI_COMM_WORLD);
}

void
Abstract_manager_node::
terminate_nodes_after_assertion(int calling_node) {
  int numtasks;
  std::cout << "terminate nodes after assertion\n";
  // get the number of tasks set at commandline (= number of processors)
  MPI_Comm_size(MPI_COMM_WORLD,&numtasks);
  for (int i=0; i<numtasks; i++) {
    if ((i!=RANK_MANAGER_NODE) && (i!=calling_node)) {
      int32_t msg=1; // 1 means error
      MPI_Send(&msg, 1, MPI_INT32, i,
               MPI_TAG_END_NODE, MPI_COMM_WORLD);
    }
  }

  // Close this node
  MPI_Barrier( MPI_COMM_WORLD );
  MPI_Finalize();

  exit(-1);
}
