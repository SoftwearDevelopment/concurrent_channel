# concurrent_channel

Simple, thread safe shell-pipe like abstraction for queues.

**REVIEWS WANTED**

## Using

Check this repository out recursively, add `include/` and
`vendor/concurrentqueue/` to your include paths.

```c++
#include "softwear/concurrent_channel.hpp"
// Holding 80K elements, polling every half millisecond
softwear::concurrent_channel<int>{80000, 500};
```

## Features

*Taken from the API documentation; in case of doubt, the API
doc is authoritative*

Beyond a basic concurrent queue functionality, this
provides flow control and end-of-transmission signaling
between threads, specifically:
* Support for a simple capacity limit
* Support for enqueue methods that block if the channel is at capacity
* Support for dequeue methods that block if no data is available in the channel
* Support for a close() method and an eof() check to
  allow provider threads to signal that there is no more
  data to process.

## Motivation

*Taken from the API documentation; in case of doubt, the API
doc is authoritative*

Moodycamel's pipe is excellent, but a bit complex to use:

**Why a simple capacity limit?**
Personally, I implemented this for an application that red
data via network, ran some heavy computation on the data
then sent it to another network host: I started a couple of
IO threads for loading, a couple to send the processed data
and a couple for the actual processing, with a channel between
each step. If fetching or the data processing where slow,
the other threads would implicitly wait for them. If the
processing or fetching where to fast, they would
automatically stop and wait for the other threads, so no
thread can fill up all the ram.

**Why close signaling?**
After the last fetching thread is done, it signals that the
job is done to processing; processing in turn can forward
the signal to the uploader, so in the management thread very
little effort is needed: It needs to set up the channel and
start the threads, then it just need to join() all the
threads and exit as soon as all the threads are don.

## Example

See tests.cc for an example.

## Documentation

`include/softwear/concurrent_queue.hpp` contains detailed
documentation.

## Testing

Run `make`.

## TODO

* Allocator support (doable through the malloc/free in the Traits)
* Locking based implementation
* More precise size computation for the internal queue
  (avoiding malloc)
* Update the documentation to reflect all information from
  Concurrentqueue
* **Reviews**

# LICENSE

Written by (karo@cupdev.net) Karolin Varner, for Softwear, BV (http://nl.softwear.nl/contact):
You can still buy me a Club Mate. Or a coffee.

Copyright © (c) 2015 and 2016, Softwear, BV.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of the Karolin Varner, Softwear, BV nor the
  names of its contributors may be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL Softwear, BV BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
