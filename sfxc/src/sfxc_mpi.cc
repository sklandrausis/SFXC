/* Copyright (c) 2007 Joint Institute for VLBI in Europe (Netherlands)
 * All rights reserved.
 *
 * Author(s): Nico Kruithof <Kruithof@JIVE.nl>, 2007
 *
 * $Id: sfxc_mpi.h 281 2007-07-10 13:53:05Z kruithof $
 *
 */


#include "sfxc_mpi.h"
#include "utils.h"
#include "log_node.h"
#include "input_node.h"
#include "output_node.h"
#include "correlator_node.h"

IF_MT_MPI_ENABLED( Mutex g_mpi_thebig_mutex );
MPI_Group MPI_GROUP_CORR_NODES;
MPI_Comm MPI_COMM_CORR_NODES;

void start_node() {
  int rank;
  // get the ID (rank) of the task, fist rank=0, second rank=1 etc.
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  MPI_Status status;
  MPI_Probe(RANK_MANAGER_NODE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
  switch (status.MPI_TAG) {
  case MPI_TAG_SET_LOG_NODE: {
      int32_t msg;
      MPI_Recv(&msg, 1, MPI_INT32,
               RANK_MANAGER_NODE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
      // No break
    }
  case MPI_TAG_LOG_MESSAGE: {

      SFXC_ASSERT (RANK_LOG_NODE == rank);
      int numtasks;
      MPI_Comm_size(MPI_COMM_WORLD,&numtasks);
      if (PRINT_PID) {
        DEBUG_MSG("Log node, pid = " << getpid());
      }
      if (PRINT_HOST) {
        char hostname[255];
        gethostname(hostname, 255);
        DEBUG_MSG("Log node, hostname = " << hostname);
      }

      Log_node log_node(rank,numtasks);
      log_node.start();
      break;
    }
  case MPI_TAG_SET_INPUT_NODE_MARK5A:
  case MPI_TAG_SET_INPUT_NODE_VLBA:
  case MPI_TAG_SET_INPUT_NODE_VDIF: 
  case MPI_TAG_SET_INPUT_NODE_MARK5B: {
      // The integer is the number of input nodes:
      int32_t station_nr;
      MPI_Recv(&station_nr, 1, MPI_INT32,
               RANK_MANAGER_NODE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
      TRANSPORT_TYPE data_format;
      if(status.MPI_TAG == MPI_TAG_SET_INPUT_NODE_MARK5B)
        data_format = MARK5B;
      else if(status.MPI_TAG == MPI_TAG_SET_INPUT_NODE_MARK5A)
        data_format = MARK5A;
      else if(status.MPI_TAG == MPI_TAG_SET_INPUT_NODE_VLBA)
        data_format = VLBA;
      else{
        SFXC_ASSERT(status.MPI_TAG == MPI_TAG_SET_INPUT_NODE_VDIF)
        data_format = VDIF;
      }
      
      if (PRINT_PID) {
        DEBUG_MSG("Input node, pid = " << getpid());
      }
      if (PRINT_HOST) {
        char hostname[255];
        gethostname(hostname, 255);
        DEBUG_MSG("Input node, hostname = " << hostname);
      }
      Time ref_date;
      int64_t clock_ticks;
      MPI_Recv(&clock_ticks, 1, MPI_INT64, RANK_MANAGER_NODE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
      ref_date.set_clock_ticks(clock_ticks);
      // Start the input node
      Input_node input_node(rank, station_nr, data_format, ref_date);
      input_node.start();
      break;
    }
  case MPI_TAG_SET_OUTPUT_NODE: {
      int32_t msg;
      MPI_Recv(&msg, 1, MPI_INT32,
               RANK_MANAGER_NODE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

      if (PRINT_PID) {
        DEBUG_MSG("Output node, pid = " << getpid());
      }
      if (PRINT_HOST) {
        char hostname[255];
        gethostname(hostname, 255);
        DEBUG_MSG("Output node, hostname = " << hostname);
      }
      Output_node node(rank);
      node.start();
      break;
    }
  case MPI_TAG_SET_CORRELATOR_NODE_PHASED:
  case MPI_TAG_SET_CORRELATOR_NODE_FILTERBANK:
  case MPI_TAG_SET_CORRELATOR_NODE_PSR_BINNING:
  case MPI_TAG_SET_CORRELATOR_NODE_BOLOMETER:
  case MPI_TAG_SET_CORRELATOR_NODE: {
      int32_t corr_nr;
      MPI_Recv(&corr_nr, 1, MPI_INT32,
               RANK_MANAGER_NODE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

      if (PRINT_PID) {
        DEBUG_MSG("Correlator node, pid = " << getpid());
      }
      if (PRINT_HOST) {
        char hostname[255];
        gethostname(hostname, 255);
        DEBUG_MSG("Correlator node, hostname = " << hostname);
      }

      // Start correlator node
      int correlator_node_type;
      switch(status.MPI_TAG) {
         case MPI_TAG_SET_CORRELATOR_NODE:
           correlator_node_type = CORRELATOR_NODE_NORMAL; 
           break;
         case MPI_TAG_SET_CORRELATOR_NODE_PHASED:
           correlator_node_type = CORRELATOR_NODE_PHASED; 
           break;
         case MPI_TAG_SET_CORRELATOR_NODE_FILTERBANK:
           correlator_node_type = CORRELATOR_NODE_FILTERBANK; 
           break;
         case MPI_TAG_SET_CORRELATOR_NODE_PSR_BINNING:
           correlator_node_type = CORRELATOR_NODE_PULSAR_BINNING;
           break;
         case MPI_TAG_SET_CORRELATOR_NODE_BOLOMETER:
           correlator_node_type = CORRELATOR_NODE_BOLOMETER;
           break;
         default:
           sfxc_abort("Invalid correlator node type received, are you mixing diffent SFXC versions?");
      }
      Correlator_node node(rank, corr_nr, correlator_node_type);
      node.start();
      break;
    }
  case MPI_TAG_END_NODE: {
      DEBUG_MSG("MPI_TAG_END_NODE");
      int32_t msg;
      MPI_Recv(&msg, 1, MPI_INT32,
               RANK_MANAGER_NODE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
      break;
    }
  default: {
      SFXC_ASSERT_MSG(false, "Unknown node type");
    }
  }
}

void end_node(int32_t rank) {
  MPI_Send(&rank, 1, MPI_INT32,
           rank, MPI_TAG_END_NODE, MPI_COMM_WORLD);
}

void create_correlator_node_comm(int nr_corr_nodes){
  // Create communicator group for the correlator nodes
  int nodes[nr_corr_nodes+1];
  int nr_nodes;

  MPI_Comm_size(MPI_COMM_WORLD, &nr_nodes);
  nodes[0] = RANK_MANAGER_NODE;
  for(int i=1;i<nr_corr_nodes+1;i++)
    nodes[i] = (nr_nodes - nr_corr_nodes) + i-1;

  MPI_Group global_group;
  MPI_Comm_group(MPI_COMM_WORLD, &global_group);
  MPI_Group_incl(global_group, nr_corr_nodes+1, nodes, &MPI_GROUP_CORR_NODES);
  MPI_Comm_create(MPI_COMM_WORLD, MPI_GROUP_CORR_NODES, &MPI_COMM_CORR_NODES);
}
