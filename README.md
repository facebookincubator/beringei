# Beringei
In the fall of 2015, we published the paper “Gorilla: A Fast, Scalable, In-Memory Time Series Database” at VLDB 2015 (http://www.vldb.org/pvldb/vol8/p1816-teller.pdf).

We are happy to announce the launch of the open source project Beringei, based on our Gorilla paper.

Beringei is a high performance time series storage engine. Time series are commonly used as a representation of statistics, gagues, and counters used primarily for performance and health monitoring. This project can be used in one of two ways. First, we have created a simple, sharded service (and reference client implementation) that can store and serve time series query requests. Second, you can use Beringei as an embedded library to handle the low-level details of efficiently storing time series data. Using Beringei in this way is similar to RocksDB - the Beringei library can be the high performance storage system underlying your performance monitoring solution.

Beringei has the following features:
* Support for very fast in-memory storage backed by disk for persistence. Queries to the storage engine are always served out of memory for extremely fast query performance, but backed to disk so the process can be restarted or migrated with very little down time and no data loss.
* Extremely efficient streaming compression algorithm. Our streaming compression algorithm that is able to compress real world time series data by over 90%. The delta of delta compression algorithm used by Beringei is also efficient - we see that a single machine is able to compress more than 1.5 million datapoints/second.
* Reference sharded service implementation, including a client implementation.
* Reference http service implementation that enables direct Grafana integration.

## Requirements
Beringei requires or works with
* Ubuntu 16.10

We also depend on these open source projects:
* fbthrift (https://github.com/facebook/fbthrift)
* folly (https://github.com/facebook/folly)
* wangle (https://github.com/facebook/wangle)
* proxygen (https://github.com/facebook/proxygen)
* gtest (https://github.com/google/googletest)
* gflags (https://github.com/gflags/gflags)

## Building Beringei
Our instructions are for Ubuntu 16.10 - but you will probably be able to modify the install scripts and directions to work with other linux distros.

Run `sudo ./setup_ubuntu.sh` (or `sudo bash setup_ubuntu.sh`, because bash is the one true shell.)

Generate the the thrift source:
```
pushd beringei/if
for THRIFT_FILE in $(ls *.thrift); do
  PYTHONPATH=/tmp/fbthrift-2016.11.07.00/thrift/.python-local/lib/python  python2 -mthrift_compiler.main --gen cpp2 $THRIFT_FILE -I../..
done
popd
```
Actually build beringei
`mkdir build && cd build && cmake .. && make`

Then:
Generate a beringei configuration file
```
./beringei/tools/beringei_configuration_generator --host_names $your_hostname --file_path /tmp/beringei.json
```
Start beringei
```
./beringei/service/beringei_main \
    -beringei_configuration_path /tmp/beringei.json \
    -create_directories \
    -sleep_between_bucket_finalization_secs 60 \
    -allowed_timestamp_behind 300 \
    -bucket_size 600 \
    -buckets $((86400/600)) \
    -logtostderr \
    -v=2
```

Send data:
```
while [[ 1 ]]; do
    ./beringei/tools/beringei_put \
        -beringei_configuration_path /tmp/beringei.json \
        testkey ${RANDOM} \
        -logtostderr -v 3
    sleep 30
done
```

Read it back:
```
./beringei/tools/beringei_get \
    -beringei_configuration_path /tmp/beringei.json \
    testkey \
    -logtostderr -v 3
```

## License
Beringei is BSD-licensed. We also provide an additional patent grant.
