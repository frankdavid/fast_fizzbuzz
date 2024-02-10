# The (perhaps) fastest FizzBuzz implementation

**283 GB/s** output on AMD Ryzen 9 7700X.

Motivation: https://codegolf.stackexchange.com/questions/215216/high-throughput-fizz-buzz

To build (tested with GCC 13):
```
g++ fizzbuzz.cc -march=native -o fizzbuzz -O3 -Wall -std=c++20 -fno-tree-vectorize -fno-exceptions
```
The build takes a few minutes to complete. Compiling with or without `-fno-tree-vectorize`
may yield better runtime performance depending on the CPU.

To benchmark (Requires installing `pv`):
```
taskset -c 0-6 ./fizzbuzz | taskset -c 7 pv -B 2M > /dev/null
```

Requires Linux 2.6.17 or later.

### Performance tuning
1. The value of the `kParallelism` constant in `fizzbuzz.cc` should be set to
available CPU cores or less.
2. The program uses `kParallelism` threads. It's worth trying different cpu 
affinities to see what gives the best performance. The number of cores assigned
by `taskset` should be equal to `kParallelism`
3. For maximum performance, [turn off mitigations](https://jcvassort.open-web.fr/how-to-disable-cpu-mitigations/#how-to-disable-these-mitigations-to-make-linux-fast-again)
(it's recommended to reenable mitigations after benchmarking since they protect
against CPU vulnerabilities).

`/proc/sys/fs/pipe-max-size` must be at least `14680064` (14MB) or alternatively
the program must be run as root (`sudo ...`)

---


## The algorithm

> I reuse some of the ideas from [ais523's answer](https://codegolf.stackexchange.com/a/236630/7251), namely:
> *  using vmsplice for zero-copy output into the pipe
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
* batch: 1,000,000 lines of consecutive output
* run: consecutive output where the line numbers have the same number of digits in base 10, eg. run(6) is the output for line numbers: 100000 ... 999999

### A few observations
**Observation 1:** within each fifteener, the number lines are always at the same indices, namely at indices 1, 2, 4, 7, 8, 11, 13 and 14

**Observation 2:** each run with 2+ digits contains a whole number of fifteeners

**Observation 3:** each run with 2+ digits starts with mod = 10 because 10^N ≡ 10 (mod 15) for N > 0

**Observation 4:** if we have 3 batches (3,000,000 lines) of output in a buffer,
we can get the next 3 batches by incrementing the 6th digit (0-indexed) from the
right of each number line by 3 in each batch. We can keep other digits untouched. We'll call
the last 6 digits of the number *suffix digits*, since these will never change in a run.
The fizz/buzz/fizzbuzz lines are also untouched.

For example the first batch of run(9) looks like this:

```
BUZZ
100000001
FIZZ
100000003
100000004
FIZZBUZZ
...
FIZZ
100999999
```

Second batch of run(9):
```
BUZZ
FIZZ
101000002
101000003
...
101999998
101999999
```

Third batch of run(9):
```
FIZZBUZZ
102000001
102000002
FIZZ
...
102999998
FIZZ
```

We can get the fourth batch by incrementing the first batch by 3,000,000:

```
BUZZ
103000001
FIZZ
103000003
103000004
FIZZBUZZ
...
FIZZ
105999999
```

Incrementing single digits is much faster than recomputing the numbers every time.

We only need to maintain three buffers for the three batches and keep incrementing numbers by 3,000,000.

It's important to note that the number lines in the buffer contain the string
representation of the numbers, eg. 103000003 is actually `['1','0','3','0','0','0','0','0','3']` = `[49, 48, 51, 48, 48, 48, 48, 48, 51]`.
Incrementing by 3,000,000 means incrementing the 6th digit (0-indexed) from the right by 3.

Using three buffers also has an addition benefit: we can put up to two buffers
into the pipe for the downstream process to read from (see vmsplice and
[this article](https://mazzo.li/posts/fast-pipes.html)) and update the third buffer in the meantime.

The basic algorithm is as follows:

	for run in 1..19:
      initialize batch0 with fizz buzz lines between 10^(run-1) and 10^(run-1) + 999,999
      output batch0
      initialize batch1 with fizz buzz lines between 10^(run-1) + 1,000,000 and 10^(run-1) + 1,999,999
      output batch1
      initialize batch2 with fizz buzz lines between 10^(run-1) + 2,000,000 and 10^(run-1) + 2,999,999
      output batch2
      for batch in 3..(number of batches in run):
        increment batch0
        output batch0
        increment batch1
        output batch1
        increment batch2
        output batch2

The algorithm is fast because the increment operation (which is where most of
the time is spent) can be optimized really well.

### Overflows and carry

A major complication in the above algorithm is when a digit overflows.
For example, if we increment the digit '8' in 108399977 by 3, the result is not a
digit, so we have to take care of the overflow.
We do this by first incrementing '8' by 3, then subtracting 10 and adding 1 to
the '0' before the '8' (which is pretty much the process how we'd do it on paper).
Furthermore, it can happen that more than even the digit before overflows, e.g. if the number is 198399977. In this case, we:

* add 3 to '8'
* subtract 10 from '8' + 3
* add 1 to '9'
* subtract 10 from '9' + 1
* add 1 to '1'

The final result is 201399977.

However, checking in each iteration whether an overflow has occurred is pretty slow.
This is where batches are useful once again. Since a batch is 1,000,000 lines of output,
all numbers in a batch share a common prefix.

    122|531269
        ------  suffix (last 6 digits)
    ---         prefix (all previous digits)
 

As mentioned above, the suffixes are never touched after the initialization.
We only increment the prefix.

The nice property of a batch is that all numbers in
a batch overflow the same way, therefore we only have to check once per chunk, how many digits
will need to be updated for each number. We call this the overflow count.

We get extra performance gains by incrementing each batch from multiple threads.
One section of a batch updated by a thread is called a **chunk**.

## C++ tricks

After discussing the algorithm, here are a few ideas that make this algorithm particularly fast:

### 8 is better than 1

Previously we talked about incrementing single characters in the buffer but CPUs can work with 8-byte integers faster than with 1-byte integers. Furthermore, if we have to update multiple digits because of overflow, updating 8 bytes at once will reduce the number of instructions.

For this to work, a requirement is that the integers must be aligned at 8 bytes, so we need to know where the 8-byte boundaries are.

Consider the number 12019839977 where we want to add 6 to the digit '8' (and handle overflow). Let's assume that the (one-byte) indexes mod 8 are as follows:


```
output:	       X Y 1 2 0 1 9 8 3 9 9 7 7
index mod 8:   0 1 2 3 4 5 6 7 0 1 2 3 4
```


`X Y` is the last two bytes before this number. Let's call the address of `X` `base`. This address is aligned to 8 bytes. Instead of updating the single bytes at (`base + 7`), (`base + 6`) and (`base + 5`), we can update the 8 bytes in a single operation using bit shifts.

On little endian systems (like x86) where the least significant byte is at the lowest address, this translates to:

```cpp
base[index \ 8] += 1 << (5 * 8)  |  (1 - 10) << (6 * 8)  |  (6 - 10) << (7 * 8)
                         ^             ^
                  index mod 8 = 5    increment by 1 - 10 (add carry and handle overflow)
```
Each update we want to do to the numbers is OR-d together. What's even better is that even if we write individual instructions, the compiler is smart enough to compile it to a single expression as long as the right handsides are compile-time constants:

```cpp
base[index \ 8] += 1 << (5 * 8);
base[index \ 8] += (1 - 10) << (6 * 8);
base[index \ 8] += (6 - 10) << (7 * 8);
```

Doing all these bit manipulations at runtime would be slower than just incrementing the numbers one byte at a time, so we'll be ...

### Using the compiler for maximum gains

All the calculation needed for the previous step to work fast is done at compile time. A few more observations:

* The first batch starts with mod 10, the second batch starts with mod 5, the batch chunk starts with mod 0.
* The first batch is aligned at 8 bytes. We can calculate the length of each batch and chunk at compile time.

Using C++ templates, we generate specialized code for each `(run digits, batch id, chunk id, overflows)` tuple.

* run digits: the number of digits of each number line in this run
* batch id: 0, 1 or 2 (see the Observation 4 above)
* chunk id: to distinguish the chunk in the batch, [0, kParallelism)
* overflow count: the number of digits that will overflow after incrementing the last digit of the prefix

In order to support the compiler in generating branchless code, we aggressively 
unroll loops so conditions and calculations can be done at compile time. The 
price is a long compile time.

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