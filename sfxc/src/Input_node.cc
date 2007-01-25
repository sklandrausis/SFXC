/* Author(s): Nico Kruithof, 2007
 * 
 * $URL:$
 * $Id: $
 */

#include <Input_node.h>

#include <types.h>
#include <Data_reader_file.h>

#include <iostream>
#include <assert.h>

Input_node::Input_node(int rank, int size) 
  : Node(rank), 
    buffer(size), 
    log_writer(0,0),     
    input(buffer, log_writer), output(buffer, log_writer)
{
  log_writer(1)<< "Input_node(rank,size)";
  add_controller(&input);
  add_controller(&output);
}

Input_node::~Input_node() {
  log_writer(1) << "~Input_node()";
}
