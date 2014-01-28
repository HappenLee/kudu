// Copyright (c) 2013, Cloudera, inc.
#include <glog/logging.h>
#include "gutil/atomicops.h"
#include <boost/thread/thread.hpp>

#include <stdlib.h>

#include "common/schema.h"
#include "common/row.h"
#include "gutil/gscoped_ptr.h"
#include "benchmarks/tpch/tpch-schemas.h"
#include "benchmarks/tpch/line_item_dao.h"
#include "benchmarks/tpch/rpc_line_item_dao.h"
#include "common/wire_protocol.h"
#include "util/status.h"
#include "common/row_changelist.h"
#include "benchmarks/tpch/line_item_tsv_importer.h"

DEFINE_string(tpch_path_to_data, "/data/3/dbgen/truncated_lineitem.tbl",
              "The full path to the '|' separated file containing the lineitem table.");
DEFINE_int32(tpch_demo_window, 3000000, "Size of the trailing windows, in terms of order numbers");
DEFINE_int32(tpch_demo_starting_point, 6000000, "Order number from which we start inserting");
DEFINE_int32(tpch_demo_updater_threads, 1 , "Number of threads that update, can be 0");
DEFINE_int32(tpch_demo_inserter_threads, 0 , "Number of threads that insert, min 0, max 1");
DEFINE_string(master_address, "localhost",
              "Address of master for the cluster to operate on");
DEFINE_int32(tpch_max_batch_size, 1000,
             "Maximum number of inserts/updates to batch at once");

const char * const kTabletId = "tpch1";

// This program is used to drive both inserts and read+mutates on the tpch
// data set.
// First, use the tpch1 insert test configured to talk to your cluster in order
// to load the initial dataset, the default start point and window are based
// on a 6GB lineitem file.
// Then, use a bigger file that's truncated up to the 6,000,000th order in
// order to insert even more data. The default path shows where that file is on
// the kudu machine a1228.
// Only 1 insert thread can be used, but many updaters can be specified.

namespace kudu {

class Demo {
 public:
  Demo(int window, int starting_point) : counter_(starting_point), window_(window) {}

  // Generate the next order, using a moving trailing window
  // The moving comes from the insert thread, no insert thread means no movement
  // The window size is configurable with tpch_demo_window
  // The order is taken at random within the window
  int GetNextOrder() {
    return (rand() % window_) + (counter_ - window_);
  }

  // Atomically replaces the current order number, thus moving the window
  void SetLastInsertedOrder(int order_number) {
    base::subtle::NoBarrier_Store(&counter_, order_number);
  }

 private:
  base::subtle::Atomic64 counter_;
  const int window_;
  DISALLOW_COPY_AND_ASSIGN(Demo);
};

// This thread continuously updates the l_quantity column from orders
// as determined by Demo::GetNextOrder. It first needs to read the order to get
// the quantity, picking the highest line number, does l_quantity+1, then
// writes it back
static void UpdateThread(Demo *demo) {
  Schema full_schema = tpch::CreateLineItemSchema();
  Schema query_schema = tpch::CreateMS3DemoQuerySchema();
  gscoped_ptr<kudu::RpcLineItemDAO> dao(new kudu::RpcLineItemDAO(FLAGS_master_address,
                                        kTabletId, FLAGS_tpch_max_batch_size));
  dao->Init();

  while (true) {
    // 1. Get the next order to update
    int current_order = demo->GetNextOrder();
    VLOG(1) << "current order: " << current_order;

    // 2. Fetch the order including the column we want to update
    ColumnRangePredicatePB pred;
    ColumnSchemaToPB(query_schema.column(0), pred.mutable_column());
    pred.mutable_lower_bound()->assign(
      reinterpret_cast<const char*>(&current_order), sizeof(current_order));
    pred.mutable_upper_bound()->assign(
      reinterpret_cast<const char*>(&current_order), sizeof(current_order));

    dao->OpenScanner(query_schema, pred);
    vector<const uint8_t*> rows;
    while (dao->HasMore()) {
      dao->GetNext(&rows);
    }
    if (rows.empty()) continue;
    ConstContiguousRow last_row(query_schema, rows.back());

    // 3. The last row has the highest line, we update it
    uint32_t l_ordernumber = *query_schema.ExtractColumnFromRow<UINT32>(last_row, 0);
    uint32_t l_linenumber = *query_schema.ExtractColumnFromRow<UINT32>(last_row, 1);
    uint32_t l_quantity = *query_schema.ExtractColumnFromRow<UINT32>(last_row, 2);
    uint32_t new_l_quantity = l_quantity + 1;

    // 4. Do the update
    VLOG(1) << "updating " << l_ordernumber << " " << l_linenumber << " "
            << l_quantity << " " << new_l_quantity;
    RowBuilder rb(full_schema.CreateKeyProjection());
    rb.AddUint32(l_ordernumber);
    rb.AddUint32(l_linenumber);
    faststring mutations;
    RowChangeListEncoder encoder(full_schema, &mutations);
    encoder.AddColumnUpdate(4, &new_l_quantity);
    dao->MutateLine(rb.row(), mutations);
  }
}

// This function inserts all the orders it reads until it runs out, and keeps
// moving the window forward
static void InsertThread(Demo *demo, const string &path) {
  gscoped_ptr<kudu::RpcLineItemDAO> dao(new kudu::RpcLineItemDAO(FLAGS_master_address,
                                        kTabletId, FLAGS_tpch_max_batch_size));
  dao->Init();
  LineItemTsvImporter importer(path);

  Schema schema(tpch::CreateLineItemSchema());
  PartialRow row(&schema);

  int order_number = importer.GetNextLine(&row);
  while (order_number != 0) {
    dao->WriteLine(row);
    // Move the window forward
    demo->SetLastInsertedOrder(order_number);
    order_number = importer.GetNextLine(&row);
  }
  dao->FinishWriting();
}

static int DemoMain(int argc, char** argv) {
  Demo demo(FLAGS_tpch_demo_window, FLAGS_tpch_demo_starting_point);
  int num_updaters = FLAGS_tpch_demo_updater_threads;
  int num_inserter = FLAGS_tpch_demo_inserter_threads;
  if (num_inserter > 1) {
    LOG(FATAL) << "Can only insert with 1 thread";
    return 1;
  }
  for (int i = 0; i < num_inserter; i++) {
    boost::thread inserter(InsertThread, &demo, FLAGS_tpch_path_to_data);
  }

  for (int i = 0; i < num_updaters; i++) {
    boost::thread flushdm_thread(UpdateThread, &demo);
  }

  while (true) {
    sleep(60);
  }
  return 0;
}
} //namespace kudu

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);

  return kudu::DemoMain(argc, argv);
}
