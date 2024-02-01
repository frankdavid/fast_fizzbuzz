# The perhaps fastest FizzBuzz implementation

Motivation: https://codegolf.stackexchange.com/questions/215216/high-throughput-fizz-buzz

To build (tested with GCC 13):
```
g++ fizzbuzz.cc -march=native -o fizzbuzz -O3 -Wall -std=c++20 -fopenmp
```
The build takes 1-2 minutes to complete. Compiling with `-mno-sse`
may yield better runtime performance depending on the CPU.

To benchmark (Requires installing `pv`):
```
taskset -c 0-2 ./fizzbuzz | taskset -c 3 pv > /dev/null
```

The program uses 3 threads, so the best is to assign 3 cores. It's worth trying different cpu affinities to see what gives the best performance.

Eg. `taskset -c 0,2,4 ./fizzbuzz | taskset -c 6 pv > /dev/null`

You need to update `/proc/sys/fs/pipe-max-size` to be at least `4194304` (4Mb) or alternatively
you can run the command as root.

Requires Linux 2.6.17 or later.

---


## The algorithm

> **Disclaimer**: I reuse some of the ideas from [ais523's answer](https://codegolf.stackexchange.com/a/236630/7251), namely:
> *  using vmsplice for zero-copy output into the pipe
> * double buffering the output
> * aligning the output buffers to 2MB and using huge pages to minimize TLB lookups
> 
> I also recommend [this great article](https://mazzo.li/posts/fast-pipes.html) about Linux pipes and vmsplice.

### Definitions



* line number: the id of each line starting with 1, 2, ...
* mod: the line number mod 15
* fizzbuzz line: one line of output
* fizzbuzz function: a function that translates the line number to a fizzbuzz line according to the fizzbuzz logic
* number line: a line of output which is a number (and not fizz, buzz or fizzbuzz)
* fifteener: 15 lines of consecutive output
* chunk: 100,000 lines of consecutive output
* half batch: 300,000 lines of consecutive output
* run: consecutive output where the line numbers have the same number of digits in base 10, eg. run(6) is the output for line numbers: 100000 ... 999999

### A few observations
**Observation 1:** within each fifteener, the number lines are always at the same indices, namely at indices 1, 2, 4, 7, 8, 11, 13 and 14

**Observation 2:** each run with 2+ digits contains a whole number of fifteeners

**Observation 3:** each run with 2+ digits starts with mod = 10 because 10^N ≡ 10 (mod 15) for N > 0

**Observation 4:** if we have a half batch (300,000 lines) of output in a buffer, we can get the next half batch by incrementing the 5th digit (0-indexed) from the right of each number line by 3. We can keep other digits untouched. We'll call the last 5 digits of the number suffix digits, since these will never change in a run.

For example the first half batch of run(8) looks like this:

```
BUZZ
10000001
FIZZ
10000003
10000004
FIZZBUZZ
...
FIZZ
10299999
```
After incrementing the number lines by 3:

```
BUZZ
10300001
FIZZ
10300003
10300004
FIZZBUZZ
...
FIZZ
10599999
```

This is exactly the second half batch, the perfect continuation of the first half batch.

It's important to note that the number lines in the buffer contain the string representation of the numbers, eg. 10300003 is actually ['1','0','3','0','0','0','0','3'] = [49, 48, 51, 48, 48, 48, 48, 51].

### If this is a half batch what is a batch?

A batch is just two half batches. In order to maximize performance, we use two buffers. While the downstream process is reading from one buffer (_output_ buffer), we concurrently update the other buffer (_update_ buffer). Because there are two half batches, each with 300,000 lines, we use increments of 6 (and not 3 like above). When we reach the end of one half batch, we swap them, output the one that we've just updated and start updating the other one.

The basic algorithm is as follows:

	for run in 1..19:
      initialize buffer1 with fizz buzz lines between 10^(run-1) and 10^(run-1) + 299,999
      output buffer1
      initialize buffer2 with fizz buzz lines between 10^(run-1) + 300,000 and 10^(run-1) + 599,999
      output buffer2
      for half_batch in 2..(number of half batches in run):
        increment buffer1
        output buffer1
        increment buffer2
        output buffer2

The algorithm is fast because the increment operation (which is where most of
the time is spent) can be optimized really well.

### Overflows and carry

A major complication in the above algorithm is when a digit overflows.
For example, if we increment the digit '8' in 10839977 by 6, the result is not a
digit, so we have to take care of the overflow.
We do this by first incrementing '8' by 6, then subtracting 10 and adding 1 to
the '0' before the '8' (which is pretty much the process how we'd do it on paper).
Furthermore, it can happen that more than even the digit before overflows, e.g. if the number is 19839977. In this case, we:

* add 6 to '8'
* subtract 10 from '8' + 6
* add 1 to '9'
* subtract 10 from '9' + 1
* add 1 to '1'

The final result is 20439977.

However, checking in each iteration whether an overflow has occurred is pretty slow.
This is where chunks come into the picture. A chunk is 100,000 lines of output,
eg. from lines 12200000 to 12299999. All numbers in a chunk share a common
prefix.

    122|53126
        -----  suffix (last 5 digits)
    ---        prefix (all previous digits)
 

As mentioned above, the suffixes are never touched in a run. We only increment the
prefix. The nice property of a chunk is that all numbers in a chunk overflow
the same way, therefore we only have to check once per chunk, how many digits
will need to be updated for each number. We call this the overflow count.


## C++ tricks

After discussing the algorithm, here are a few ideas that make this algorithm particularly fast:

### 8 is better than 1

Previously we talked about incrementing single characters in the buffer but CPUs can work with 8-byte integers faster than with 1-byte integers. Furthermore, if we have to update multiple digits because of overflow, updating 8 bytes at once will reduce the number of instructions.

For this to work, a requirement is that the integers must be aligned at 8 bytes, so we need to know where the 8-byte boundaries are.

Consider the number 12019839977 where we want to add 6 to the digit '8' (and handle overflow). Let's assume that the (one-byte) indexes mod 8 are as follows:


```
number:	       X Y 1 2 0 1 9 8 3 9 9 7 7
index mod 8:   0 1 2 3 4 5 6 7 0 1 2 3 4
```


`X Y` is the last two bytes before this number. Let's call the address of `X` `base`. This address is aligned to 8 bytes. Instead of updating the single bytes at (`base + 7`), (`base + 6`) and (`base + 5`), we can update the 8 bytes in a single operation using bit shifts.

On little endian systems (like x86) where the least significant byte is at the lowest address, this translates to:

```cpp
base[index \ 8] += 1 << (5 * 8)  |  (1 - 10) << (6 * 8)  |  (6 - 10) << (7 * 8)
                         ^             ^
                  index mod 8 = 5    increment by 1 - 10 (add carry and handle overflow)
```
Each update we want to do to the numbers is OR-d together. What's even better is that even if we write individual instructions, the compiler is smart enough to compile it to a single expression as long as the right handside is a compile-time constant.

```cpp
base[index \ 8] += 1 << (5 * 8);
base[index \ 8] += (1 - 10) << (6 * 8);
base[index \ 8] += (6 - 10) << (7 * 8);
```

Doing all these bit manipulations at runtime would be slower than just incrementing the numbers one byte at a time, so we'll be ...

### Using the compiler for maximum gains

All the calculation needed for the previous step to work fast is done at compile time. A few more observations:

* A half batch contains 3 chunks.
* The first chunk starts with mod 10, the second chunk starts with mod 5, the third chunk starts with mod 0.
* The first chunk is aligned at 8 bytes. We can calculate the length of each chunk at compile time using the number of digits. Note that since chunks don't contain a whole number of fifteeners, the three chunks will have three different sizes.

Using C++ templates, we generate specialized code for each (run digits, chunk id, overflows) triplet.

* run digits: the number of digits of each number line in this run
* chunk id: to distinguish the chunk in the half batch, 0, 1 or 2
* overflow count: the number of digits that overflow after incrementing the last digit of the prefix

In order to support the compiler in generating branchless code, we aggressively unroll loops so conditions and calculations can be done at compile time. The price is a long compile time.

If we inspect the generated assembly, we can see that the compiler generates
specialized code which only contains add/sub instructions without any branches.

```asm
add	QWORD PTR 8[rax], rdx
sub	QWORD PTR 40[rax], 1033
add	QWORD PTR 32[rax], rdx
add	QWORD PTR 56[rax], r8
sub	QWORD PTR 88[rax], 4
add	QWORD PTR 80[rax], rsi
sub	QWORD PTR 104[rax], 67698432
sub	QWORD PTR 128[rax], 67698432
sub	QWORD PTR 160[rax], 4
[many more add/sub]
```

Most of the time, we only need 8 instructions for each fifteener.

The 3 chunks can be updated parallelly from 3 threads, e.g. with openmp (hand coding the parallel execution may lead to further performance gains, I haven't tried it).