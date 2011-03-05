/*
 * Copyright (c) 2009 Mark Rages
 * Copyright (c) 2011 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "ANTChannel.h"
#include <QDebug>

static float timeout_blanking=2.0;  // time before reporting stale data, seconds
static float timeout_drop=2.0; // time before reporting dropped message
static float timeout_scan=10.0; // time to do initial scan
static float timeout_lost=30.0; // time to do more thorough scan

ANTChannel::ANTChannel(int number, ANT *parent) : number(number), parent(parent)
{
    channel_type=CHANNEL_TYPE_UNUSED;
    channel_type_flags=0;
    is_cinqo=0;
    is_old_cinqo=0;
    control_channel=NULL;
    manufacturer_id=0;
    product_id=0;
    product_version=0;
    device_number=0;
    channel_assigned=0;
    state=ANT_UNASSIGN_CHANNEL;
    blanked=1;
    messages_received=0;
    messages_dropped=0;
    setId();
    srm_offset=0;
    burstInit();
}

//
// channel id is in the form nnnnx where nnnn is the device number
// and x is the channel type (p)ower, (c) adence etc the full list
// can be found in ANT.cpp when initialising ant_sensor_types
//
void ANTChannel::setId()
{
    if (channel_type==CHANNEL_TYPE_UNUSED) {
        strcpy(id, "none");
    } else {
        snprintf(id, 10, "%d%c", device_number, parent->ant_sensor_types[channel_type].suffix);
    }
}

// The description of the device
const char * ANTChannel::getDescription()
{
    return parent->ant_sensor_types[channel_type].descriptive_name;
}

// Get device type
int ANTChannel::interpretDescription(char *description)
{
    const ant_sensor_type_t *st=parent->ant_sensor_types;

    do {
        if (0==strcmp(st->descriptive_name,description))
            return st->type;
    } while (++st, st->type != CHANNEL_TYPE_GUARD);
    return -1;
}

// Open an ant channel assignment.
void ANTChannel::open(int device, int chan_type)
{
    channel_type=chan_type;
    channel_type_flags = CHANNEL_TYPE_QUICK_SEARCH ;
    device_number=device;

    setId();

    if (channel_assigned) {
        parent->sendMessage(ANTMessage::unassignChannel(number));
    } else {
        attemptTransition(ANT_UNASSIGN_CHANNEL);
    }
}

// close an ant channel assignment
void ANTChannel::close()
{
    lostInfo();
    lastMessage = ANTMessage();
    parent->sendMessage(ANTMessage::close(number));
}

//
// The main read loop is in ANT.cpp, it will pass us
// the inbound message received for our channel.
// XXX fix this up to re-use ANTMessage for decoding
// all the inbound messages
//
void ANTChannel::receiveMessage(unsigned char *ant_message)
{
    switch (ant_message[2]) {
    case ANT_CHANNEL_EVENT:
        channelEvent(ant_message);
        break;
    case ANT_BROADCAST_DATA:
        broadcastEvent(ant_message);
        break;
    case ANT_ACK_DATA:
        ackEvent(ant_message);
        break;
    case ANT_CHANNEL_ID:
        channelId(ant_message);
        break;
    case ANT_BURST_DATA:
        burstData(ant_message);
        break;
    default:
        break; //XXX should trap error here, but silently ignored for now
    }

    if (get_timestamp() > blanking_timestamp + timeout_blanking) {
        if (!blanked) {
            blanked=1;
            staleInfo(); //XXX does nothing for now....
        }
    } else blanked=0;
}

// static helper to conver message codes to an english string
// when outputing diagnostics for received messages
static const char *errormessage(unsigned char c)
{
    switch (c) {
    case 0 : return "No error";
    case 1 : return "Search timeout";
    case 2 : return "Message RX fail";
    case 3 : return "Event TX";
    case 4 : return "Receive TX fail";
    case 5 : return "Ack or Burst completed";
    case 6 : return "Event transfer TX failed";
    case 7 : return "Channel closed success";
    case 8 : return "dropped to search after missing too many messages.";
    case 9 : return "Channel collision";
    case 10 : return "Burst starts";
    case 21 : return "Channel in wrong state";
    case 22 : return "Channel not opened";
    case 24 : return "Open without valid id";
    case 25 : return "OpenRXScan when other channels open";
    case 31 : return "Transmit whilst transfer in progress";
    case 32 : return "Sequence number out of order";
    case 33 : return "Burst message past sequence number not transmitted";
    case 40 : return "INVALID PARAMETERS";
    case 41 : return "INVALID NETWORK";
    case 48 : return "ID out of bounds";
    case 49 : return "Transmit during scan mode";
    case 64 : return "NVM for SensRciore mode is full";
    case 65 : return "NVM write failed";
    default: return "UNKNOWN MESSAGE CODE";
    }
}

// process a channel event message
// XXX should re-use ANTMessage rather than
// raw message data
void ANTChannel::channelEvent(unsigned char *ant_message) {

    unsigned char *message=ant_message+2;

    if (MESSAGE_IS_RESPONSE_NO_ERROR(message)) {

        attemptTransition(RESPONSE_NO_ERROR_MESSAGE_ID(message));

    } else if (MESSAGE_IS_EVENT_CHANNEL_CLOSED(message)) {

        parent->sendMessage(ANTMessage::unassignChannel(number));

    } else if (MESSAGE_IS_EVENT_RX_SEARCH_TIMEOUT(message)) {

        // timeouts are normal for search channel
        if (channel_type_flags & CHANNEL_TYPE_QUICK_SEARCH) {

        channel_type_flags &= ~CHANNEL_TYPE_QUICK_SEARCH;
        channel_type_flags |= CHANNEL_TYPE_WAITING;

        } else {

            lostInfo(); //XXX does nothing for now

            channel_type=CHANNEL_TYPE_UNUSED;
            channel_type_flags=0;
            device_number=0;
            setId();

            parent->sendMessage(ANTMessage::unassignChannel(number));
        }

        //XXX channel_manager_start_waiting_search(self->parent);

    } else if (MESSAGE_IS_EVENT_RX_FAIL(message)) {

        messages_dropped++;
        double t=get_timestamp();

        if (t > (last_message_timestamp + timeout_drop)) {
            if (channel_type != CHANNEL_TYPE_UNUSED) dropInfo();
            // this is a hacky way to prevent the drop message from sending multiple times
            last_message_timestamp+=2*timeout_drop;
        }

    } else if (MESSAGE_IS_EVENT_RX_ACKNOWLEDGED(message)) {

        exit(-10);

    } else if (MESSAGE_IS_EVENT_TRANSFER_TX_COMPLETED(message)) {

        if (tx_ack_disposition) {} //XXX tx_ack_disposition();

    } else {

        // Not very friendly but useful for now!
        fprintf(stderr, "WARNING type=%x channel=%x id=%x code=%s\n", *message, *(message+1), *(message+2), errormessage(*(message+3)));
    }
}

// if this is a quarq cinqo then record that fact
// was probably interesting to quarqd, but less so for us!
void ANTChannel::checkCinqo()
{

    int version_hi, version_lo, swab_version;
    version_hi=(product_version >> 8) &0xff;
    version_lo=(product_version & 0xff);

    swab_version=version_lo | (version_hi<<8);

    if (!(mi.first_time_manufacturer || mi.first_time_product)) {
        if ((product_id == 1) && (manufacturer_id==7)) {
            // we are a cinqo, were we aware of this?
            is_cinqo=1;

            // are we an old-version or new-version cinqo?
            is_old_cinqo = ((version_hi <= 17) && (version_lo==10));
            //XXX channel_manager_associate_control_channels(self->parent);
        }
    }
}

// notify we have a cinqo, does nothing
void ANTChannel::sendCinqoSuccess() {}

//
// We got a broadcast event -- this is where inbound
// telemetry gets processed, and for many message types
// we need to remember previous messages to look at the
// deltas during the period XXX this needs fixing!
//
void ANTChannel::broadcastEvent(unsigned char *ant_message)
{

    unsigned char *message=ant_message+2;
    double timestamp=get_timestamp();

    messages_received++;
    last_message_timestamp=timestamp;

    if (state != MESSAGE_RECEIVED) {
        // first message! who are we talking to?
        parent->sendMessage(ANTMessage::requestMessage(number, ANT_CHANNEL_ID));
        blanking_timestamp=get_timestamp();
        blanked=0;
        return; // because we can't associate a channel id with the message yet
    }

    // for automatically opening quarq channel on early cinqo
    if (MESSAGE_IS_PRODUCT(message)) {

        mi.first_time_product= false;
        product_version&=0x00ff;
        product_version|=(PRODUCT_SW_REV(message))<<8;
        checkCinqo();

    } else if (MESSAGE_IS_MANUFACTURER(message)) {

        mi.first_time_manufacturer= false;
        product_version&=0xff00;
        product_version|=MANUFACTURER_HW_REV(message);
        manufacturer_id=MANUFACTURER_MANUFACTURER_ID(message);
        product_id=MANUFACTURER_MODEL_NUMBER_ID(message);
        checkCinqo();

    } else {

        //
        // We got some telemetry on this channel
        //
        ANTMessage antMessage(parent, ant_message);

        if (lastMessage.type != 0) {

           switch (channel_type) {

           // Power
           case CHANNEL_TYPE_POWER:
           case CHANNEL_TYPE_QUARQ:
           case CHANNEL_TYPE_FAST_QUARQ:
           case CHANNEL_TYPE_FAST_QUARQ_NEW:

                // what kind of power device is this?
                switch(antMessage.power_type) {

                //
                // SRM - crank torque frequency
                //
                case ANT_CRANKSRM_POWER: // 0x20 - crank torque (SRM)
                {
                    int period = antMessage.period - lastMessage.period;
                    int torque = antMessage.torque - lastMessage.torque;
                    float time = (float)period / (float)2000.00;

                    if (time && antMessage.slope && period) {

                        nullCount = 0;
                        float torque_freq = torque / time - 428/*srm_offset*/; //XXX need to support calibration
                        float nm_torque = 10.0 * torque_freq / antMessage.slope;
                        float cadence = 2000.0 * 60 * (antMessage.eventCount - lastMessage.eventCount) / period;
                        float power = 3.14159 * nm_torque * cadence / 30;

                        // ignore the occassional spikes XXX is this a boundary error on event count ?
                        if (power > 0 && power < 2501 && cadence >0 && cadence < 256) {
                            parent->setWatts(power);
                            parent->setCadence(cadence);
                        }

                    } else {

                        nullCount++;
                        if (nullCount >= 4) { // 4 messages on an SRM
                            parent->setWatts(0);
                            parent->setCadence(0);
                        }
                    }
                }
                break;

                //
                // Powertap - wheel torque
                //
                case ANT_WHEELTORQUE_POWER: // 0x11 - wheel torque (Powertap)
                {
                    int events = antMessage.eventCount - lastMessage.eventCount;
                    int period = antMessage.period - lastMessage.period;
                    int torque = antMessage.torque - lastMessage.torque;

                    if (events && period) {

                        nullCount = 0;

                        float nm_torque = torque / (32.0 * events);
                        float wheelRPM = 2048.0 * 60.0 * events / period;
                        float power = 3.14159 * nm_torque * wheelRPM / 30;

                        parent->setWheelRpm(wheelRPM);
                        parent->setWatts(power);

                    } else {
                        nullCount++;

                        if (nullCount >= 4) { // 4 messages on Powertap ? XXX validate this
                            parent->setWheelRpm(0);
                            parent->setWatts(0);
                        }
                    }
                }
                break;

                //
                // Standard Power
                //
                case ANT_STANDARD_POWER: // 0x10 - standard power
                {
                    int events = antMessage.eventCount - lastMessage.eventCount;
                    if (events) {
                        nullCount =0;
                        parent->setWatts(antMessage.instantPower);
                    } else {
                        nullCount++;

                        if (nullCount >= 4) { // 4 messages on Standard Power ? XXX validate this
                            parent->setWatts(0);
                        }
                    }
                }
                break;


                //
                // Quarq - Crank torque
                //
                case ANT_CRANKTORQUE_POWER: // 0x12 - crank torque (Quarq)
                {
                    int events = antMessage.eventCount - lastMessage.eventCount;
                    int period = antMessage.period - lastMessage.period;
                    int torque = antMessage.torque - lastMessage.torque;

                    if (events && period) {
                        nullCount = 0;

                        float nm_torque = torque / (32.0 * events);
                        float cadence = 2048.0 * 60.0 * events / period;
                        float power = 3.14159 * nm_torque * cadence / 30;

                        parent->setCadence(cadence);
                        parent->setWatts(power);

                    } else {
                        nullCount++;
                        if (nullCount >= 4) { //XXX 4 on a quarq??? validate this
                            parent->setCadence(0);
                            parent->setWatts(0);
                        }
                    }
                }
                break;

                default: // UNKNOWN POWER DEVICE? XXX Garmin (Metrigear) Vector????
                    break;
            }
            break;

           // HR
           case CHANNEL_TYPE_HR:
           {
               // cadence first...
               int time = antMessage.measurementTime - lastMessage.measurementTime;
               if (time) {
                   nullCount = 0;
                   parent->setBPM(antMessage.instantHeartrate);
               } else {
                   nullCount++;
                   if (nullCount >= 12) parent->setBPM(0); // 12 according to the docs
               }
           }
           break;

           // Cadence
           case CHANNEL_TYPE_CADENCE:
           {
               int time = antMessage.crankMeasurementTime - lastMessage.crankMeasurementTime;
               int revs = antMessage.crankRevolutions - lastMessage.crankRevolutions;
               if (time) {
                   float cadence = 1024*60*revs / time;
                   parent->setCadence(cadence);
               }
           }
           break;

           // Speed and Cadence
           case CHANNEL_TYPE_SandC:
           {
               // cadence first...
               int time = antMessage.crankMeasurementTime - lastMessage.crankMeasurementTime;
               int revs = antMessage.crankRevolutions - lastMessage.crankRevolutions;
               if (time) {
                   nullCount = 0;
                   float cadence = 1024*60*revs / time;
                   parent->setCadence(cadence);
               } else {
                   nullCount++;
                   if (nullCount >= 12) parent->setCadence(0);
               }

               // now speed ...
               time = antMessage.wheelMeasurementTime - lastMessage.wheelMeasurementTime;
               revs = antMessage.wheelRevolutions - lastMessage.wheelRevolutions;
               if (time) {
                   dualNullCount = 0;

                   float rpm = 1024*60*revs / time;
                   parent->setWheelRpm(rpm);
               } else {

                    dualNullCount++;
                    if (dualNullCount >= 12) parent->setWheelRpm(0);
               }
           }
           break;

           // Speed
           case CHANNEL_TYPE_SPEED:
           {
               int time = antMessage.wheelMeasurementTime - lastMessage.wheelMeasurementTime;
               int revs = antMessage.wheelRevolutions - lastMessage.wheelRevolutions;
               if (time) {
                   nullCount=0;
                   float rpm = 1024*60*revs / time;
                   parent->setWheelRpm(rpm);
               } else {
                   nullCount++;
                   if (nullCount >= 12) parent->setWheelRpm(0);
               }
           }
           break;

           default:
             break; // unknown?
           }

        } else {

            // reset nullCount if receiving first telemetry update
            dualNullCount = nullCount = 0;
        }
        lastMessage = antMessage;
    }
}

// we got an acknowledgement, so lets process it
// does nothing for now
void ANTChannel::ackEvent(unsigned char * /*ant_message*/) { }


// we got a channel ID notification
void ANTChannel::channelId(unsigned char *ant_message) {

    unsigned char *message=ant_message+2;

    device_number=CHANNEL_ID_DEVICE_NUMBER(message);
    device_id=CHANNEL_ID_DEVICE_TYPE_ID(message);
    state=MESSAGE_RECEIVED;

    setId();
    channelInfo();

  // if we were searching,
    if (channel_type_flags & CHANNEL_TYPE_QUICK_SEARCH) {
        parent->sendMessage(ANTMessage::setSearchTimeout(number, (int)(timeout_lost/2.5)));
    }
    channel_type_flags &= ~CHANNEL_TYPE_QUICK_SEARCH;

    //XXX channel_manager_start_waiting_search(self->parent);
    // if we are quarq channel, hook up with the ant+ channel we are connected to
    //XXX channel_manager_associate_control_channels(self->parent);
}

// get ready to burst
void ANTChannel::burstInit() {
    rx_burst_data_index=0;
    rx_burst_next_sequence=0;
    rx_burst_disposition=NULL;
}

// are we in the middle of a search?
int ANTChannel::isSearching() {
    return ((channel_type_flags & (CHANNEL_TYPE_WAITING | CHANNEL_TYPE_QUICK_SEARCH)) || (state != MESSAGE_RECEIVED));
}


// receive burst data
void ANTChannel::burstData(unsigned char *ant_message) {

    unsigned char *message=ant_message+2;
    char seq=(message[1]>>5)&0x3;
    char last=(message[1]>>7)&0x1;
    const unsigned char next_sequence[4]={1,2,3,1};

    if (seq!=rx_burst_next_sequence) {
        // XXX handle errors
    } else {

        int len=ant_message[ANT_OFFSET_LENGTH]-3;

        if ((rx_burst_data_index + len)>(RX_BURST_DATA_LEN)) {
            len = RX_BURST_DATA_LEN-rx_burst_data_index;
        }

        rx_burst_next_sequence=next_sequence[(int)seq];
        memcpy(rx_burst_data+rx_burst_data_index, message+2, len);
        rx_burst_data_index+=len;
    }

    if (last) {
        if (rx_burst_disposition) {
          //XXX what does this do? rx_burst_disposition();
        }
        burstInit();
    }
}

void ANTChannel::attemptTransition(int message_id)
{

    const ant_sensor_type_t *st;
    int previous_state=state;
    st=&(parent->ant_sensor_types[channel_type]);

    // update state
    state=message_id;

    // do transitions
    switch (state) {

    case ANT_CLOSE_CHANNEL:
//qDebug()<<"transition ... close channel...";
        // next step is unassign and start over
        // but we must wait until event_channel_closed
        // which is its own channel event
        state=MESSAGE_RECEIVED;
        break;

    case ANT_UNASSIGN_CHANNEL:
//qDebug()<<"transition ... unassign channel...";
        channel_assigned=0;
        if (st->type==CHANNEL_TYPE_UNUSED) {
            // we're shutting the channel down
        } else {
            device_id=st->device_id;
            if (channel_type & CHANNEL_TYPE_PAIR) {
                device_id |= 0x80;
            }
            setId();
//qDebug()<<"st network is"<<st->network;
            parent->sendMessage(ANTMessage::assignChannel(number, 0, st->network)); // recieve channel on network 1
        }
        break;

    case ANT_ASSIGN_CHANNEL:
//qDebug()<<"transition ... assign channel...";
        channel_assigned=1;
        parent->sendMessage(ANTMessage::setChannelID(number, device_number, device_id, 0));
        break;

    case ANT_CHANNEL_ID:
//qDebug()<<"transition ... channel id...";
        if (channel_type & CHANNEL_TYPE_QUICK_SEARCH) {
            parent->sendMessage(ANTMessage::setSearchTimeout(number, (int)(timeout_scan/2.5)));
        } else {
            parent->sendMessage(ANTMessage::setSearchTimeout(number, (int)(timeout_lost/2.5)));
        }
        break;

    case ANT_SEARCH_TIMEOUT:
//qDebug()<<"transition ... search timeout...";
        if (previous_state==ANT_CHANNEL_ID) {
            // continue down the intialization chain
            parent->sendMessage(ANTMessage::setChannelPeriod(number, st->period));
        } else {
            // we are setting the ant_search timeout after connected
            // we'll just pretend this never happened
            state=previous_state;
        }
        break;

    case ANT_CHANNEL_PERIOD:
//qDebug()<<"transition ... channel period...";
        parent->sendMessage(ANTMessage::setChannelFreq(number, st->frequency));
        break;

    case ANT_CHANNEL_FREQUENCY:
//qDebug()<<"transition ... channel frequency...";
        parent->sendMessage(ANTMessage::open(number));
        mi.initialise();
        break;

    case ANT_OPEN_CHANNEL:
//qDebug()<<"transition ... open channel...";
        break;

    default:
        break;
    }
}

// refactored out XXX fix this
int ANTChannel::setTimeout(char * /*type*/, float /*value*/, int /*connection*/) { return 0; }

// These should emit signals to notify the channel manager
// but for now we just ignore XXX fix this
void ANTChannel::searchComplete() {}
void ANTChannel::reportTimeouts() {}
void ANTChannel::staleInfo() {}
void ANTChannel::lostInfo() {}
void ANTChannel::dropInfo() {}
void ANTChannel::channelInfo() {}

//
// Calibrate... XXX not used at present
//
// request the device on this channel calibrates itselt
void ANTChannel::requestCalibrate() {
  parent->sendMessage(ANTMessage::requestCalibrate(number));
}