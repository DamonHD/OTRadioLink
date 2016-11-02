/*
The OpenTRV project licenses this file to you
under the Apache Licence, Version 2.0 (the "Licence");
you may not use this file except in compliance
with the Licence. You may obtain a copy of the Licence at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the Licence is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied. See the Licence for the
specific language governing permissions and limitations
under the Licence.

Author(s) / Copyright (s): Damon Hart-Davis 2016
*/

/*
 * OTRadValve TempControl tests.
 */

#include <gtest/gtest.h>
#include <stdio.h>

#include "OTSIM900Link.h"


// Test for general sanity of OTSIM900Link.
// Make sure that an instance can be created and does not die horribly.
// Underlying simulated serial/SIM900 never accepts data or responds, eg like a dead card.
TEST(OTSIM900Link,basicsDeadCard)
{
    const bool verbose = false;

    class NULLSerialStream final : public Stream
      {
      public:
        void begin(unsigned long) { }
        void begin(unsigned long, uint8_t);
        void end();

        virtual size_t write(uint8_t c) override { if(verbose) { fprintf(stderr, "%c\n", (char) c); } return(0); }
        virtual int available() override { return(-1); }
        virtual int read() override { return(-1); }
        virtual int peek() override { return(-1); }
        virtual void flush() override { }
      };
    OTSIM900Link::OTSIM900Link<0, 0, 0, NULLSerialStream> l0;
    EXPECT_TRUE(l0.begin());
    EXPECT_EQ(OTSIM900Link::GET_STATE, l0._getState());
    // Try to hang just by calling poll() repeatedly.
    for(int i = 0; i < 100; ++i) { l0.poll(); }
    EXPECT_GE(OTSIM900Link::START_UP, l0._getState()) << "should keep trying to start with GET_STATE, RETRY_GET_STATE and START_UP";
    // ...
    l0.end();
}

// Test for general sanity of OTSIM900Link.
// Make sure that an instance can be created and does not die horribly.
// Underlying simulated serial/SIM900 accepts output, does not respond.
namespace B1
{
const bool verbose = true;

// Does a trivial simulation of SIM900, responding to start of 'A' of AT command.
class TrivialSimulator final : public Stream
  {
  public:
    // Events exposed.
    static bool haveSeenCommandStart;

  private:
    // Command being collected from OTSIM900Link.
    bool waitingForCommand = true;
    bool collectingCommand = false;
    // Entire request starting "AT"; no trailing CR or LF stored.
    std::string command;

    // Reply (postfix) being returned to OTSIM900Link: empty if none.
    std::string reply;

  public:
    void begin(unsigned long) { }
    void begin(unsigned long, uint8_t);
    void end();

    virtual size_t write(uint8_t uc) override
      {
      const char c = (char)uc;
      if(waitingForCommand)
        {
        // Look for leading 'A' of 'AT' to start a command.
        if('A' == c)
          {
          waitingForCommand = false;
          collectingCommand = true;
          command = 'A';
          haveSeenCommandStart = true; // Note at least one command start.
          }
        }
      else
        {
        // Look for CR (or LF) to terminate a command.
        if(('\r' == c) || ('\n' == c))
          {
          waitingForCommand = true;
          collectingCommand = false;
          if(verbose) { fprintf(stderr, "command received: %s\n", command.c_str()); }
          // Respond to particular commands...
          if("AT" == command) { reply = "AT\r"; }
          // DHD20161101: "No PIN" response (deliberately not typical SIM900 response) resulted in SIGSEGV from not checking getResponse() result for NULL.
          else if("AT+CPIN?" == command) { reply = "No PIN"; }
          }
        else if(collectingCommand) { command += c; }
        }
      if(verbose) { if(isprint(c)) { fprintf(stderr, "<%c\n", c); } else { fprintf(stderr, "< %d\n", (int)c); } }
      return(1);
      }
    virtual int read() override
        {
        if(0 == reply.size()) { return(-1); }
        const char c = reply[0];
        if(verbose) { if(isprint(c)) { fprintf(stderr, ">%c\n", c); } else { fprintf(stderr, "> %d\n", (int)c); } }
        reply.erase(0, 1);
        return(c);
        }
    virtual int available() override { return(-1); }
    virtual int peek() override { return(-1); }
    virtual void flush() override { }
  };
// Events exposed.
bool TrivialSimulator::haveSeenCommandStart;
}
TEST(OTSIM900Link,basics)
{
//    const bool verbose = true;

    ASSERT_FALSE(B1::TrivialSimulator::haveSeenCommandStart);
    OTSIM900Link::OTSIM900Link<0, 0, 0, B1::TrivialSimulator> l0;
    EXPECT_TRUE(l0.begin());
    EXPECT_EQ(OTSIM900Link::GET_STATE, l0._getState());
    // Try to hang just by calling poll() repeatedly.
    for(int i = 0; i < 100; ++i) { l0.poll(); }
    EXPECT_TRUE(B1::TrivialSimulator::haveSeenCommandStart) << "should see some attempt to communicate with SIM900";
    EXPECT_LE(OTSIM900Link::WAIT_FOR_REGISTRATION, l0._getState()) << "should make it to at least WAIT_FOR_REGISTRATION";
    // ...
    l0.end();
}


