/* Author(s): Nico Kruithof, 2007
 * 
 * $URL:$
 * $Id: $
 */

#ifndef INPUT_NODE_H
#define INPUT_NODE_H

#include <Node.h>
#include <Input_controller.h>
#include <Output_controller.h>
#include <Semaphore_buffer.h>
#include <Ring_buffer.h>

#include "Log_writer_cout.h"

class Input_node : public Node {
public:
  Input_node(int rank, int buffer_size = 1024);
  ~Input_node();
  
private:
  //Ring_buffer<Input_controller::value_type> buffer;
  Semaphore_buffer<Input_controller::value_type> buffer;

  Log_writer_cout log_writer;

  Input_controller input;
  Output_controller output;
};

#endif // INPUT_NODE_H
