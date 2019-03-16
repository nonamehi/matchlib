/*
 * Copyright (c) 2017-2019, NVIDIA CORPORATION.  All rights reserved.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License")
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __AXI_T_SLAVE_FROM_FILE__
#define __AXI_T_SLAVE_FROM_FILE__

#include <systemc.h>
#include <ac_reset_signal_is.h>

#include <axi/axi4.h>
#include <axi/testbench/CSVFileReader.h>
#include <nvhls_connections.h>
#include <hls_globals.h>

#include <queue>
#include <map>
#include <boost/assert.hpp>
#include <algorithm>
#include <string>
#include <sstream>

/**
 * \brief An AXI slave with its memory prepopulated from a file for use in testbenches.
 * \ingroup AXI
 *
 * \tparam axiCfg                   A valid AXI config.
 *
 * \par Overview
 * AxiSlaveFromFile acts as an AXI slave in a testbench.  It differs from Slave in that at the beginning of simulation time its internal state is prepopulated according to the contents of a CSV file.  The CSV file format is as follows:
 * - address_in_hex,data_in_hex
 * 
 *  It's best to specify the full DATA_WIDTH of data.
 *
 */
template <typename axiCfg> class SlaveFromFile : public sc_module {
 public:
  static const int kDebugLevel = 0;
  typedef axi::axi4<axiCfg> axi4_;

  typename axi4_::read::slave if_rd;
  typename axi4_::write::slave if_wr;

  sc_in<bool> reset_bar;
  sc_in<bool> clk;

  std::queue <typename axi4_::ReadPayload> rd_resp;
  std::queue < sc_uint<axi4_::ADDR_WIDTH> > rd_resp_addr;
  std::queue <typename axi4_::AddrPayload> wr_addr;
  std::queue <typename axi4_::WritePayload> wr_data;
  std::queue <typename axi4_::WRespPayload> wr_resp;

  std::map< sc_uint<axi4_::ADDR_WIDTH>, sc_uint<axi4_::DATA_WIDTH> > localMem;
  std::map< sc_uint<axi4_::ADDR_WIDTH>, sc_uint<8> > localMem_wstrb;
  std::vector< sc_uint<axi4_::ADDR_WIDTH> > validReadAddresses;

  typename axi4_::WritePayload load_data_pld;

  static const int bytesPerBeat = axi4_::DATA_WIDTH >> 3;

  SC_HAS_PROCESS(SlaveFromFile);

  SlaveFromFile(sc_module_name name_, std::string filename="mem.csv")
      : sc_module(name_), if_rd("if_rd"), if_wr("if_wr"), reset_bar("reset_bar"), clk("clk") {

    CSVFileReader reader(filename);
    std::vector< std::vector<std::string> > dataList = reader.readCSV();
    for (unsigned int i=0; i < dataList.size(); i++) {
      std::vector<std::string> vec = dataList[i];
      NVHLS_ASSERT(vec.size() == 2);
      std::stringstream ss;
      sc_uint<axi4_::ADDR_WIDTH> addr;
      ss << hex << vec[0];
      ss >> addr;
      std::stringstream ss_data;
      sc_uint<axi4_::DATA_WIDTH> data;
      ss_data << hex << vec[1];
      ss_data >> data;
      load_data_pld.data = data;
      if (axiCfg::useWriteStrobes) {
        for (int j=0; j<axi4_::WSTRB_WIDTH; j++) {
          localMem_wstrb[addr+j] = static_cast< sc_bv<8> >(load_data_pld.data.range(8*j+7,8*j));
        }
      } else {
        localMem[addr] = load_data_pld.data;
      }  
      validReadAddresses.push_back(addr);
    }

    SC_CTHREAD(run_rd, clk.pos());
    async_reset_signal_is(reset_bar, false);

    SC_CTHREAD(run_wr, clk.pos());
    async_reset_signal_is(reset_bar, false);
  }

protected:
  void run_rd() {
    if_rd.reset();
    unsigned int rdBeatInFlight = 0;

    while (1) {
      wait();

      typename axi4_::AddrPayload rd_addr_pld;
      if (if_rd.nb_aread(rd_addr_pld)) {
        sc_uint<axi4_::ADDR_WIDTH> addr = rd_addr_pld.addr;
        CDCOUT(sc_time_stamp() << " " << name() << " Received read request:"
                      << " addr=" << hex << addr
                      << " burstlen=" << dec << (axiCfg::useBurst ? rd_addr_pld.len : "N/A")
                      << endl, kDebugLevel);
        sc_uint<axi4_::ALEN_WIDTH> len = (axiCfg::useBurst ? rd_addr_pld.len : 0);
        for (unsigned int i=0; i<(len+1); i++) {
          std::ostringstream msg;
          msg << "\nError @" << sc_time_stamp() << " from " << name()
              << ": Received a read request from an address that has not yet been written to"
              << ", addr=" << hex << addr
              << endl;
          bool validAddr = false;
          for (unsigned int j=0; j<validReadAddresses.size(); j++) {
            if (validReadAddresses[j] == rd_addr_pld.addr) validAddr = true;
          }
          BOOST_ASSERT_MSG( validAddr, msg.str().c_str() );
          typename axi4_::ReadPayload data_pld;
          if (axiCfg::useWriteStrobes) {
            for (int k=0; k<axi4_::WSTRB_WIDTH; k++) {
              if (localMem_wstrb.find(addr+k) != localMem_wstrb.end()) {
                data_pld.data.range(8*k+7,8*k) = localMem_wstrb[addr+k];
              } else {
                data_pld.data.range(8*k+7,8*k) = 0;
              }
            }
          } else {
            data_pld.data = localMem[addr];
          }
          data_pld.resp = axi4_::Enc::XRESP::OKAY;
          data_pld.id = rd_addr_pld.id;
          data_pld.last = (i == len);
          rd_resp.push(data_pld);
          rd_resp_addr.push(addr);
          addr += bytesPerBeat;
        }
      }

      if (!rd_resp.empty()) {
        typename axi4_::ReadPayload data_pld;
        data_pld = rd_resp.front();
        sc_uint<axi4_::ADDR_WIDTH> addr = rd_resp_addr.front();
        if (if_rd.nb_rwrite(data_pld)) {
          CDCOUT(sc_time_stamp() << " " << name() << " Returned read data:"
                        << " data=" << hex << data_pld.data.to_uint64()
                        << " addr=" << hex << addr.to_uint64()
                        << " beat=" << dec << (axiCfg::useBurst ? static_cast< sc_uint<32> >(rdBeatInFlight++) : "N/A")
                        << endl, kDebugLevel);
          if (data_pld.last == 1) {
            rdBeatInFlight = 0;
          }
          rd_resp.pop();
          rd_resp_addr.pop();
        }
      }
    }
  }

  void run_wr() {
    if_wr.reset();

    typename axi4_::WRespPayload resp_pld;
    typename axi4_::AddrPayload wr_addr_pld;
    typename axi4_::WritePayload wr_data_pld;
    unsigned int wrBeatInFlight = 0;
    bool first_beat = 1;
    sc_uint<axi4_::ADDR_WIDTH> wresp_addr;

    while (1) {
      wait();

      // Send a write response out of the local queue
      if (axiCfg::useWriteResponses) {
        if (!wr_resp.empty()) {
          resp_pld = wr_resp.front();
          if (if_wr.nb_bwrite(resp_pld)) {
            wr_resp.pop();
            CDCOUT(sc_time_stamp() << " " << name() << " Sent write response"
                          << endl, kDebugLevel);
          }
        }
      }

      // Grab a write request (addr) and put it in the local queue
      if (if_wr.aw.PopNB(wr_addr_pld)) {
        wr_addr.push(wr_addr_pld);
        CDCOUT(sc_time_stamp() << " " << name() << " Received write request:"
                      << " addr=" << hex << wr_addr_pld.addr.to_uint64()
                      << endl, kDebugLevel);
      }

      // Grab a write request (data) and put it in the local queue
      if (if_wr.w.PopNB(wr_data_pld)) {
        wr_data.push(wr_data_pld);
        CDCOUT(sc_time_stamp() << " " << name() << " Received write data:"
                      << " data=" << hex << wr_data_pld.data.to_uint64()
                      << " wstrb=" << hex << (axiCfg::useWriteStrobes ? wr_data_pld.wstrb : "N/A")
                      << " beat=" << dec << (axiCfg::useBurst ? static_cast< sc_uint<32> >(wrBeatInFlight++) : "N/A")
                      << endl, kDebugLevel);
        if (wr_data_pld.last == 1) {
          wrBeatInFlight = 0;
        }
      }

      // Handle a write request in the local queues
      if (!wr_addr.empty() & !wr_data.empty()) {
        if (first_beat) {
          wr_addr_pld = wr_addr.front();
          wresp_addr = wr_addr_pld.addr;
          first_beat = 0;
        }
        sc_uint<axi4_::ALEN_WIDTH> len = (axiCfg::useBurst ? wr_addr_pld.len : 0);
        wr_data_pld = wr_data.front(); wr_data.pop();
        // Store the data
        if (axiCfg::useWriteStrobes) {
          std::ostringstream msg;
          msg << "\nError @" << sc_time_stamp() << " from " << name()
              << ": Wstrb cannot be all zeros" << endl;
          BOOST_ASSERT_MSG( wr_data_pld.wstrb != 0, msg.str().c_str() );
          for (int j=0; j<axi4_::WSTRB_WIDTH; j++) {
            if (wr_data_pld.wstrb[j] == 1) {
              localMem_wstrb[wresp_addr+j] = static_cast< sc_bv<8> >(wr_data_pld.data.range(8*j+7,8*j));
            }
          }
        } else {
          localMem[wresp_addr] = wr_data_pld.data;
        }
        validReadAddresses.push_back(wresp_addr);
        wresp_addr += bytesPerBeat;
        if (wr_data_pld.last == 1) {
          wr_addr.pop();
          first_beat = 1;
          // Generate a response
          if (axiCfg::useWriteResponses) {
            resp_pld.resp = axi4_::Enc::XRESP::OKAY;
            resp_pld.id = wr_addr_pld.id;
            wr_resp.push(resp_pld);
          }
        }
      }
    }
  }
};

#endif
