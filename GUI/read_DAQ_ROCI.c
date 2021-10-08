#include <stdio.h>
#include <memory.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <iostream>
#include <iomanip>
#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "GetPot"

#include "NIDAQmx.h"

// Shorthand for the ASIO library namespace
using boost::asio::ip::tcp;

const int DAQ_LISTEN_PORT = 9000;

const int ID_CHIPCONF = 100;
const int ID_TIMESTAMP = 101;
const int ID_HITDATA = 102;

#ifdef __linux__
#else
int32 error=0;
#define DAQmxErrChk(functionCall) if( DAQmxFailed(error=(functionCall)) ) goto Error; else

struct DAQError : public std::exception
{
	int _error;
	char _errBuff[4096];
	DAQError(int error) : _error(error) {
		DAQmxGetExtendedErrorInfo(_errBuff,sizeof(_errBuff));
	}
#if __GNUG__
	const char* what() const throw () {
#else
	const char* what() const {
#endif
		return _errBuff;
	}
};
#define DAQmxErrCheck(functionCall) if ( DAQmxFailed(error=(functionCall)) ) throw DAQError(error); 
#endif

int take_data = 1;
int bytesOut = 0;
int maxEvt = 0;
bool printData = false;
bool roc = false;
int default_chipVersion = 2;
int chipVersion = default_chipVersion;
double default_MHz = 5.0;

// Optional BCO value filter -- use with extreme caution!!!
int bco_min = -1;
int bco_max = 9999;

const int32 nmax=8192;
int32 bufCnt = 0;

// Global file-writing mutex
boost::mutex mut;

// Global double-ended queue for buffering online monitoring data
// Also needs to be protected by an access mutex
//
boost::mutex onlmon_mut;
std::deque<boost::shared_ptr<uInt32> > onlmon_data;

void
decode_ROCv1_FPHXv1(const int32 word, int& adc, int& bco, int& ampl, int& chan_id, int& fpga, int& chipid)
{
	// ROCv1 output (FPHX-1) format
	// ----------------------------
	// DDDB BBBB BAAA AAA? CCCC CCC? FIII II??
	// same key as version-2...
	//
	adc     = word>>29 & 0x07;
	bco     = word>>23 & 0x3f;
	ampl    = word>>17 & 0x3f;
	chan_id = word>>9  & 0x7f;
	fpga    = word>>7  & 0x01;
	chipid  = word>>2  & 0x1f;
	return;
}

void
decode_ROCv1_FPHXv2(const int32 word, int& adc, int& bco, int& ampl, int& chan_id, int& fpga, int& chipid)
{
	// ROCv1 output (FPHX-2) format
	// ----------------------------
	// DDxB BBBB BAAA AAA1 CCCC CCBC FIII IID0
	// adc = DDD, bco = BBBBBB, ampl = AAAAAA, channel = CCCC CCCC, 
	// chip id = IIIII
	adc     = ((word>>30) & 0x03) | ((word<<1) & 0x04);
	bco     = ((word>>23) & 0x3F) | ((word>>3) & 0x40);
	ampl    =  (word>>17) & 0x3F;
	chan_id = ((word>>10) & 0x3F) | ((word>>2) & 0x40);
	fpga    =  (word>>7)  & 0x01;
	chipid  =  (word>>2)  & 0x1F;
	return;
}

void
push_data(boost::shared_ptr<uInt32> data)
{
	boost::lock_guard<boost::mutex> lock(onlmon_mut);
	onlmon_data.push_back(data);
}

boost::shared_ptr<uInt32>
pop_data(boost::shared_ptr<uInt32> data)
{
	boost::lock_guard<boost::mutex> lock(onlmon_mut);
	boost::shared_ptr<uInt32> tmp = onlmon_data.front();
	onlmon_data.pop_front();
	return tmp;
}

void
GracefulTerminate(int signal)
{
	printf("Application gracefully terminating...\n");
	take_data = 0;
}

uInt32
WriteRecord(FILE* f, int id, int cnt, uInt32* data)
{
	// A record consists of 32-bit words, with format defined as:
	//
	// word[0] = N-1, packet length excluding word count
	// word[1] = packet ID
	// word[2...N-2] = N-3 data words
	// word[N-1] checksum of word[0..N-2]
	//
	// The checksum is calculated in this routine.

	// Scoped lock to have thread block until main thread lets it go
	boost::lock_guard<boost::mutex> lock(mut);

	uInt32 cksum = 0;
	uInt32 len = cnt+3; // wordcount + id + data + checksum
	uInt32* buf = new uInt32[len];
	boost::scoped_array<uInt32> tmp(buf); // automatic memory deletion at end of scope
	buf[0] = len-1;
	buf[1] = id;
	std::copy(data,data+cnt,buf+2);
	for (uInt32 i=0; i<len-1; i++) cksum ^= buf[i];
	buf[len-1] = cksum;

	int32 nwrote = (int32)fwrite(buf, sizeof(uInt32), len, f);
	if ( nwrote != len ) printf("WARNING: Short write: wanted %d words, wrote %d words\n",len,nwrote);
	bufCnt++;
	return(nwrote*sizeof(uInt32));
}

uInt32 
WriteTime (FILE *f1, int32 curTime) 
{
	uInt32 writeBuf[2] = { 0 };
	//uInt32 cksum =0;
	int32 cnt = 2;
	memset(writeBuf, 0, sizeof(writeBuf));
	writeBuf[0] = curTime;
	writeBuf[1] = CLOCKS_PER_SEC;
	//cksum = 0;
	//for (int32 i = 0; i<4; i++) cksum ^= writeBuf[i];
	int nwrot = WriteRecord(f1,ID_TIMESTAMP,cnt,writeBuf);
	fflush(f1); // force a flush every timestamp
	return nwrot;
}

uInt32 
WriteData(FILE *f1, int cnt, uInt32 *data1)
{
	return WriteRecord(f1,ID_HITDATA,cnt,data1);
}

void
RecordTime(int32 startTime, FILE* fp)
{
	int32 lastTime = startTime;
	while ( take_data )
	{
		int32 curTime = clock();
		if ((curTime -  lastTime)/CLOCKS_PER_SEC >= 300){
			bytesOut += WriteTime(fp, curTime);
			std::cout << "Time into Run: " << (curTime-startTime)/CLOCKS_PER_SEC/60 << " minutes" << std::endl;
			lastTime = curTime;
		}

		// Check if interrupt was requested while waiting for mutex to become available
		if ( boost::this_thread::interruption_requested() ) return;

		// Sleep for a small amount to free up the CPU
		boost::this_thread::sleep(boost::posix_time::milliseconds(5000));
	}
}



std::size_t	
read_msg(tcp::socket& socket, std::vector<uInt32>& msg, boost::system::error_code& ec)
{
	unsigned int len = 0;
	std::size_t n = boost::asio::read(socket, boost::asio::buffer(&len,sizeof(len)),boost::asio::transfer_all(), ec);
	if (ec)
	{
		// An error occurred.
		std::cout << "Error occurred: " << ec.message() << std::endl;
		return 0;
	}
	std::cout << "read " << n << " bytes, val = " << len << std::endl;
	msg.clear();
	msg.resize(len);
	n = boost::asio::read(socket,boost::asio::buffer(msg),boost::asio::transfer_all(), ec);

	return n;
}
void
ControlServer(FILE* fp, tcp::socket* socket)
{
	tcp::acceptor acceptor(socket->get_io_service(), tcp::endpoint(tcp::v4(), DAQ_LISTEN_PORT));

	std::cout << "Listen for incoming connections" << std::endl;
	//tcp::socket socket(io_service);
	acceptor.accept(*socket);

	std::cout << "Connection accepted" << std::endl;

	while (take_data)
	{
		std::vector<uInt32> msg;
		boost::system::error_code error;
		std::size_t len = read_msg(*socket,msg,error);

		std::cout << "Got " << len << " bytes" << std::endl;

		if (error == boost::asio::error::eof)
		{
			// Connection closed cleanly by peer.
			std::cout << "EOF, quitting control thread" << std::endl;
			take_data = 0;
			break;
		}
		else if (error)
		{
			std::cout << "ControlServer: Error occurred, " << error.message() << std::endl;
			take_data = 0;
			break;
			//throw boost::system::system_error(error); // Some other error.
		}
		else
		{
			//uInt32 chksum = 0;
			//for(unsigned int i=0; i<msg.size(); i++) chksum ^= msg[i];
			bytesOut += WriteRecord(fp,ID_CHIPCONF,msg.size(),&msg[0]);
		}
	}
}

void
AcquireData(std::string ports, std::string pfi, double sampleHz, FILE* fp)
{
#ifdef __linux__
#else
	std::cout << "Acquiring data on " << ports << " with " << pfi << std::endl;

	/*********************************************/
	// DAQmx Configure Code
	/*********************************************/
	TaskHandle taskHandle=0;
	uInt32 data[1000000];
	int32 cnt = 0;
	//int32 cksum = 0;
	uInt32  data1[8192];
	try
	{
		DAQmxErrCheck (DAQmxCreateTask("",&taskHandle));
		DAQmxErrCheck (DAQmxCreateDIChan(taskHandle,ports.c_str(),"",DAQmx_Val_ChanForAllLines));
		DAQmxErrCheck (DAQmxCfgSampClkTiming(taskHandle,"",sampleHz,
			DAQmx_Val_Rising,DAQmx_Val_FiniteSamps,1000000));
		DAQmxErrCheck (DAQmxExportSignal(taskHandle,DAQmx_Val_SampleClock,pfi.c_str()));

		/*********************************************/
		// DAQmx Read Code
		/*********************************************/

		int32 ibuf = 0;
		int32 chan_id = 0;
		int32 evtcnt = 0;
		int32 numRead, index;
		uInt16 first_word, second_word;

		while ( take_data )	{
			DAQmxErrCheck (DAQmxStartTask(taskHandle));
			DAQmxErrCheck (DAQmxReadDigitalU32(taskHandle,-1,10.0,DAQmx_Val_GroupByChannel,
				data,1000000,&numRead,NULL));
			DAQmxErrCheck (DAQmxStopTask(taskHandle));

			for (index = 0; index<1000000; index++) {
				if ( roc )
				{
					if ((data[index]&0x0000FFFF)!=0) 
					{
						evtcnt++;
						if ( (data[index]&0x0000001) == 0) //first half of 32 bit word
						{    
							first_word = (data[index]&0x0000FFFF);
							int row     =((data[index]>>10) & 0x3F) | ((data[index]>>2) & 0x40);
							int chip_id = (data[index]>>2) & 0x1F;
							if ( !printData && index<5){
								boost::posix_time::ptime t(boost::date_time::microsec_clock<boost::posix_time::ptime>::local_time());
								std::string tstr = boost::posix_time::to_simple_string(t);
								std::cout << tstr << " ";
								std::cout << "FPGA 0, chip_id " << std::setw(2) << chip_id
									<< " chan " << std::setw(3) << row
									<< ", 0x" << std::hex << std::setw(4) << std::setfill('0') << first_word
									<< std::dec << std::setfill(' ') << std::endl;
							}
						}
						else
						{         
							data1[cnt] = ((data[index]&0x0000FFFF)<<16) | first_word;
							int32 word = data1[cnt];
							
							cnt++;
							if (printData)	
							{
								int adc, bco, ampl, chan_id, fpga, chipid;
								if ( chipVersion == 2 )
								{
									// ROCv1 output (FPHX-2) format
									// ----------------------------
									// DDxB BBBB BAAA AAA1 CCCC CCBC FIII IID0
									// adc = DDD, bco = BBBBBB, ampl = AAAAAA, channel = CCCC CCCC, 
									// chip id = IIIII
									adc     = ((word>>30) & 0x03) | ((word<<1) & 0x04);
									bco     = ((word>>23) & 0x3F) | ((word>>3) & 0x40);
									ampl    =  (word>>17) & 0x3F;
									chan_id = ((word>>10) & 0x3F) | ((word>>2) & 0x40);
									fpga    =  (word>>7)  & 0x01;
									chipid  =  (word>>2)  & 0x1F;
								}
								else if ( chipVersion == 1 )
								{
									// ROCv1 output (FPHX-1) format
									// ----------------------------
									// DDDB BBBB BAAA AAA? CCCC CCC? FIII II??
									// same key as above...
									//
									adc     = word>>29 & 0x07;
									bco     = word>>23 & 0x3f;
									ampl    = word>>17 & 0x3f;
									chan_id = word>>9  & 0x7f;
									fpga    = word>>7  & 0x01;
									chipid  = word>>2  & 0x1f;
								}
								if ( (bco_min <= bco) && (bco <= bco_max) )
								{
									boost::posix_time::ptime t(boost::date_time::microsec_clock<boost::posix_time::ptime>::local_time());
									std::string tstr = boost::posix_time::to_simple_string(t);
									std::cout << tstr << " ";

									std::cout << "FPGA" << std::setw(1) << fpga << ": " 
										<< "chipid " << std::setw(3) << chipid 
										<< " chan " << std::setw(3) << chan_id
										<< " amp " << ampl 
										<< " adc " << adc 
										<< " bco " << std::setw(3) << bco
										<< " packet 0x" << std::setw(8) << std::setfill('0') << std::hex << word
										<< std::setfill(' ') << std::dec << std::endl;
								}
							}
						}
					}

					if ((data[index]&0xFFFF0000)!=0) 
					{
						evtcnt++;
						if ( (data[index]&0x00010000) == 0) //second half of 32 bit word
						{    
							second_word = ((data[index]&0xFFFF0000)>>16);
							//int row = (second_word&0xfe00)>>9;
							//int chip_id = (second_word&0x7c)>>2;	
							int row     =((second_word>>10) & 0x3F) | ((second_word>>2) & 0x40);
							int chip_id = (second_word>>2) & 0x1F;
							if ( !printData && index<5){
								boost::posix_time::ptime t(boost::date_time::microsec_clock<boost::posix_time::ptime>::local_time());
								std::string tstr = boost::posix_time::to_simple_string(t);
								std::cout << tstr << " ";
								std::cout << "FPGA 1, chip_id " << std::setw(2) << chip_id
									<< " chan " << std::setw(3) << row
									<< ", 0x" << std::hex << std::setw(4) << std::setfill('0') << first_word
									<< std::dec << std::setfill(' ') << std::endl;
							}
						}
						else
						{     
							// Use the highest bit of the chip_id to tag which FPGA this data
							// is coming from (8th bit):
							data1[cnt] = ((data[index]&0xFFFF0000) | (second_word)) | 0x00000080;
							int32 word = data1[cnt];
							cnt++;

							if (printData)	
							{
								int adc, bco, ampl, chan_id, fpga, chipid;
								if ( chipVersion == 2 )
								{
									decode_ROCv1_FPHXv2(word, adc, bco, ampl, chan_id, fpga, chipid);
								}
								else if ( chipVersion == 1 )
								{
									decode_ROCv1_FPHXv1(word, adc, bco, ampl, chan_id, fpga, chipid);
								}
								if ( (bco_min <= bco) && (bco <= bco_max) )
								{
									boost::posix_time::ptime t(boost::date_time::microsec_clock<boost::posix_time::ptime>::local_time());
									std::string tstr = boost::posix_time::to_simple_string(t);
									std::cout << tstr << " ";

									std::cout << "FPGA" << std::setw(1) << fpga << ": " 
										<< "chipid " << std::setw(3) << chipid 
										<< " chan " << std::setw(3) << chan_id
										<< " amp " << ampl 
										<< " adc " << adc 
										<< " bco " << std::setw(3) << bco
										<< " packet 0x" << std::setw(8) << std::setfill('0') << std::hex << word
										<< std::setfill(' ') << std::dec << std::endl;
								}
							}
						}
					}
				}
				else
				{
					// Spartan3 format data
					if ((data[index]&0x0FFFFFFF)!=0) {
						if ((data[index]&0x00000001)==0) continue;
						data1[cnt] = data[index];
						cnt++;

						evtcnt++;
						if (maxEvt>0 && evtcnt>maxEvt) take_data = 0;

						if (printData) {
							/*int32 bco = (data[index] & 0x3F000000) >> 24 ;
							int32 chan_id = (data[index] & 0x0003F800) >> 11;
							int32 adc = (data[index] & 0x00380000) >> 19;
							int32 ampl = (data[index] & 0x000007FE) >> 1;*/
							int32 bco     = (data[index]>>11) & 0x0000003F;
							int32 chan_id = (data[index]>>18) & 0x0000003F;
							int32 adc     = (data[index]>>26) & 0x00000007;
							int32 ampl    = (data[index]>>1)  & 0x000003FF;
							std::cout << "chan = " << std::setw(3) << chan_id
								<< "   ampl = " << ampl 
								<< "   adc = " << adc 
								<< "   bco = " << std::setw(2) << bco
								<< "   packet = 0x" << std::setw(8) << std::setfill('0') << std::hex << data[index]
							<< std::setfill(' ') << std::dec << std::endl;
						} else if ( (evtcnt < 50) ||
							(evtcnt < 1000 && evtcnt%100 == 0) ||
							(evtcnt < 10000 && evtcnt%1000 == 0) ||
							evtcnt%10000 == 0 )
							std::cout << "event: " << evtcnt << std::endl;
					}
				}

				if (cnt >= nmax - 2) {
					bytesOut += WriteData(fp, cnt, data1);
					ibuf = 0;
					cnt = 0;
					//cksum = 0;
				}
			} 
		}
	}
	catch (const DAQError& e)
	{
		std::cout << "DAQmx error caught: " << e.what() << std::endl;
	}
	catch (const std::exception& e)
	{
		std::cout << "Caught exception: " << e.what() << std::endl;
	}
	catch (...)
	{
		std::cout << "Caught unhandled exception... " << std::endl;
	}

	if (cnt > 0) {
		bytesOut += WriteData(fp, cnt, data1);
		fflush(fp);
	}

	if( taskHandle!=0 ) {
		std::cout << "Stopping DAQ code" << std::endl;
		/*********************************************/
		// DAQmx Stop Code
		/*********************************************/
		DAQmxStopTask(taskHandle);
		DAQmxClearTask(taskHandle);
		std::cout << "Stopped DAQ code" << std::endl;
	}
#endif
}

int 
main(int argc, char **argv)
{
	// GetPot is a portable options processor
	GetPot cl(argc,argv);

	chipVersion = cl.follow(default_chipVersion,"-v");
	printData = cl.search("-p");
	double rateMHz = cl.follow(default_MHz,"-s");
	maxEvt = cl.follow(-1,"-e");
	std::string fname = cl.follow("","-f");
	roc = cl.search("-r");
	bco_min = cl.follow(bco_min,"-bco_min");
	bco_max = cl.follow(bco_max,"-bco_max");

	// Some error detection 
	const char* known_opts[] = {
		"-p", "-s", "-e", "-f", "-r", "-v", "-bco_min", "-bco_max"
	};
	std::vector<std::string> opts(known_opts,known_opts+8);
	std::vector<std::string> badopts = cl.unidentified_options(opts);
	if ( badopts.size() > 0 ) 
	{
		std::cout << "Invalid options: ";
		std::copy(badopts.begin(),badopts.end(),std::ostream_iterator<std::string>(std::cout," "));
		std::cout << std::endl;
		std::cout << "Command format: read_DAQ [OPTION]..." << std::endl;
		std::cout << "Arguments: -f <filename>   Output file name, supersedes default name" << std::endl;
		std::cout << "           -e <max events> Maximum number of events to process (0 no limit, D)" << std::endl;
		std::cout << "           -p              Print to screen (Default is no print)" << std::endl;
		std::cout << "           -r              Read out ROC-formatted data, default is false" << std::endl;
		std::cout << "           -s <rate>       Sampling rate in MHz (Default is " << default_MHz << ")" << std::endl;
		std::cout << "           -v <version>    FPHX version to unpack for print (Default is " << default_chipVersion << ")" << std::endl;
		std::cout << "Press any key to exit" << std::endl;
		getchar();
		return 1;
	}

	double sampleHz = rateMHz * 1e6; // Convert rate in MHz to Hz

	// Set defaults before parsing command line
	// File name
	char fileName[4096]={'\0'}; 
	time_t now = time(&now);
	struct tm* timeinfo = localtime(&now);
	sprintf(fileName, "c:/mannel/fphx_raw_%2.2d%2.2d%2.2d-%2.2d%2.2d.dat",
		timeinfo->tm_mday, timeinfo->tm_mon+1, 
		timeinfo->tm_year-100, timeinfo->tm_hour, timeinfo->tm_min);
	if ( !fname.size() )
		fname = std::string(fileName);

	std::cout << "Opening file: " << fname << std::endl;
	FILE* f1 = fopen(fname.c_str() , "wb"); 

	int32 startTime = clock();
	int32 lastTime = startTime;
	bytesOut = WriteTime(f1, startTime);
	
	std::cout << "Using sampling rate of " << sampleHz << " Hz" << std::endl;
	std::cout << "Taking " << maxEvt << " events" << std::endl;
	std::cout << "Print to screen: " << printData << std::endl;
	std::cout << "Reading from ROC: " << roc << std::endl;
	std::cout << "FPHX chip version: " << chipVersion << std::endl;
	std::cout << "Filter BCO range = " << bco_min << " .. " << bco_max << std::endl;

	// Configure the signal handling
	signal(SIGINT,GracefulTerminate);
	signal(SIGTERM,GracefulTerminate);

	std::cout << "Create deferred lock on DAQ mutex" << std::endl;
	boost::unique_lock<boost::mutex> lock(mut,boost::defer_lock_t());

	// TODO: add these to the commandline arguments
	const int NCARD = 1;
	const char* ports[NCARD] = { "Dev1/port0:3" };
	const char* pfi[NCARD] = { "/Dev1/PFI2" };

	// NB: Do not delete these pointers.  boost::thread_group will
	// delete them in its dtor, so if we delete them, it will be an error
	// 
	boost::thread* timestamp_thread;
	boost::thread* ctrl_socket_thread;
	boost::thread* data_threads[NCARD] = { 0 };
	boost::thread_group threads;

	// Create a server socket and and associate it with an i/o service.
	boost::asio::io_service io_service;
	tcp::socket ctrl_socket(io_service);

	try 
	{
		// Create the control server thread with the socket
		ctrl_socket_thread = new boost::thread(ControlServer,f1,&ctrl_socket);
		threads.add_thread(ctrl_socket_thread);

		// Create the thread that writes timestamps to the file
		timestamp_thread = new boost::thread(RecordTime,startTime,f1);
		threads.add_thread(timestamp_thread);

		// Create the thread(s) to read the NI DAQ card(s)
		for (unsigned int i=0; i<NCARD; i++)
		{
			data_threads[i] = new boost::thread(AcquireData,ports[i],pfi[i],sampleHz,f1);
			threads.add_thread(data_threads[i]);
		}

		// An alternative to a no-op loop might be to just join_all() and wait 
		// for the threads to react to take_data going to zero.  Might be err-prone, though.
		//
		while ( take_data )
		{
			// Sleep for a small amount to free up the CPU
			boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
		}
	}
	catch (...)
	{
		// TODO: exception handling
		std::cout << "Caught unhandled exception" << std::endl;
	}

	std::cout << "Shutting down threads" << std::endl;
	threads.interrupt_all();
	std::cout << "Releasing lock if necessary" << std::endl;
	if ( lock.owns_lock() ) lock.unlock();
	// Test if the socket is still open; if so, close it.
	std::cout << "Force socket closed" << std::endl;
	if ( ctrl_socket.is_open() ) ctrl_socket.close();
	std::cout << "Joining all threads" << std::endl;
	threads.join_all();

	int32 curTime = clock();
	bytesOut += WriteTime(f1, curTime);
	std::cout << "Time into Run: " << ((curTime - startTime)/CLOCKS_PER_SEC/60) << " minutes "
		<< ((curTime - startTime)/CLOCKS_PER_SEC % 60) << " sec" << std::endl;
	std::cout << "EOF Dumping data, total of " << bufCnt << " buffers and " << bytesOut << " bytes" << std::endl;
	fclose(f1);
	std::cout << "Output file = " << fname << std::endl;

	std::cout << "End of program, press Enter key to quit" << std::endl;
	getchar();
	std::cout << "Goodbye" << std::endl;
	return 0;
}
