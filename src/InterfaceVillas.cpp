/* Copyright 2017-2020 Institute for Automation of Complex Power Systems,
 *                     EONERC, RWTH Aachen University
 * DPsim
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *********************************************************************************/

#include <stdexcept>
#include <cstdio>
#include <cstdlib>

#include <spdlog/sinks/stdout_color_sinks.h>

#include <dpsim-villas/InterfaceVillas.h>
#include <cps/Logger.h>

using namespace CPS;
using namespace DPsim;

InterfaceVillas::InterfaceVillas(const String &name, const String &nodeType, const String &nodeConfig, UInt queueLenght, UInt sampleLenght, UInt downsampling) :
	InterfaceSampleBased(name, name, true, downsampling), // Set sync=true for all InterfaceVillas instances
	mNodeType(nodeType),
	mNodeConfig(nodeConfig),
	mQueueLenght(queueLenght),
	mSampleLenght(sampleLenght)
	{
	
	node::NodeType* nodeTypeStruct = node::node_type_lookup(mNodeType);
	if (nodeTypeStruct != nullptr) {
		mNode = std::make_unique<node::Node>(nodeTypeStruct);
		int ret = 0;

		json_error_t error;
		json_t* config = json_loads(mNodeConfig.c_str(), 0, &error);
		if (config == nullptr) {
			throw JsonError(config, error);
		}

		uuid_t fakeSuperNodeUUID;
		uuid_generate_random(fakeSuperNodeUUID);
		ret = mNode->parse(config, fakeSuperNodeUUID);
		if (ret < 0) {
			mLog->error("Error: Node in InterfaceVillas failed to parse config. Parse returned code {}", ret);
			std::exit(1);
		}
		ret = mNode->check();
		if (ret < 0) {
			mLog->error("Error: Node in InterfaceVillas failed check. Check returned code {}", ret);
			std::exit(1);
		}

		ret = node::memory::init(100);
		if (ret)
			throw RuntimeError("Error: VillasNode failed to initialize memory system");

		//villas::kernel::rt::init(priority, affinity);

		ret = node::pool_init(&mSamplePool, mQueueLenght, sizeof(Sample) + SAMPLE_DATA_LENGTH(mSampleLenght));
		if (ret < 0) {
			mLog->error("Error: InterfaceVillas failed to init sample pool. pool_init returned code {}", ret);
			std::exit(1);
		}

		ret = mNode->prepare();
		if (ret < 0) {
			mLog->error("Error: Node in InterfaceVillas failed to prepare. Prepare returned code {}", ret);
			std::exit(1);
		}
	} else {
		mLog->error("Error: NodeType {} is not known to VILLASnode!", mNodeType);
		std::exit(1);
	}
}

void InterfaceVillas::open(CPS::Logger::Log log) {
	mLog = log;
	mLog->info("Opening InterfaceVillas...");

	int ret = node::node_type_start(mNode->getType(), nullptr); //We have no SuperNode, so just hope type_start doesnt use it...
	if (ret)
		throw RuntimeError("Failed to start node-type: {}", *mNode->getType());

	ret = mNode->start();
	if (ret < 0) {
		mLog->error("Fatal error: failed to start node in InterfaceVillas. Start returned code {}", ret);
		close();
		std::exit(1);
	}
	mOpened = true;

	mSequence = 0;
	mLastSample = node::sample_alloc(&mSamplePool);
	mLastSample->signals = mNode->getInputSignals(false);
	mLastSample->sequence = 0;
	mLastSample->ts.origin.tv_sec = 0;
	mLastSample->ts.origin.tv_nsec = 0;

	std::memset(&mLastSample->data, 0, mLastSample->capacity * sizeof(float));

}

void InterfaceVillas::close() {
	mLog->info("Closing InterfaceVillas...");
	int ret = mNode->stop();
	if (ret < 0) {
		mLog->error("Error: failed to stop node in InterfaceVillas. Stop returned code {}", ret);
		std::exit(1);
	}
	mOpened = false;
	ret = node::pool_destroy(&mSamplePool);
	if (ret < 0) {
		mLog->error("Error: failed to destroy SamplePool in InterfaceVillas. pool_destroy returned code {}", ret);
		std::exit(1);
	}
}

void InterfaceVillas::readValues(bool blocking) {
	Sample *sample = nullptr; //FIXME: sample needs to be initialized to call mNode->read
	int ret = 0;
	try {
		while (ret == 0)
			ret = mNode->read(&sample, 1);
		if (ret < 0) {
			mLog->error("Fatal error: failed to read sample from InterfaceVillas. Read returned code {}", ret);
			close();
			std::exit(1);
		}

		for (auto imp : mImports) {
			imp(sample);
		}

		sample_decref(sample);
	}
	catch (std::exception& exc) {
		/* probably won't happen (if the timer expires while we're still reading data,
		 * we have a bigger problem somewhere else), but nevertheless, make sure that
		 * we're not leaking memory from the queue pool */
		if (sample)
			sample_decref(sample);

		throw exc;
	}
}

void InterfaceVillas::writeValues() {
	Sample *sample = nullptr;
	Int ret = 0;
	bool done = false;
	try {

		sample = node::sample_alloc(&mSamplePool);
		sample->signals = mNode->getInputSignals(false);

		for (auto exp : mExports) {
			exp(sample);
		}

		sample->sequence = mSequence++;
		sample->flags |= (int) villas::node::SampleFlags::HAS_DATA;
		clock_gettime(CLOCK_REALTIME, &sample->ts.origin);
		done = true;

		do {
			ret = mNode->write(&sample, 1);
		} while (ret == 0);
		if (ret < 0)
			mLog->error("Failed to write samples to InterfaceVillas. Write returned code {}", ret);

		sample_copy(mLastSample, sample);
	}
	catch (std::exception& exc) {
		/* We need to at least send something, so determine where exactly the
		 * timer expired and either resend the last successfully sent sample or
		 * just try to send this one again.
		 * TODO: can this be handled better? */
		if (!done)
			sample = mLastSample;

		while (ret == 0)
			ret = mNode->write(&sample, 1);

		if (ret < 0)
			mLog->error("Failed to write samples to InterfaceVillas. Write returned code {}", ret);

		/* Don't throw here, because we managed to send something */
	}
}
