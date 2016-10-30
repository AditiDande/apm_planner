/*===================================================================
APM_PLANNER Open Source Ground Control Station

(c) 2016 APM_PLANNER PROJECT <http://www.ardupilot.com>

This file is part of the APM_PLANNER project

    APM_PLANNER is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    APM_PLANNER is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with APM_PLANNER. If not, see <http://www.gnu.org/licenses/>.

======================================================================*/
/**
 * @file BinLogParser.cpp
 * @author Arne Wischmann <wischmann-a@gmx.de>
 * @date 14 Jul 2016
 * @brief File providing implementation for the binary log parser
 */

#include "BinLogParser.h"
#include "logging.h"

BinLogParser::typeDescriptor::typeDescriptor() : m_ID(0xFF), m_length(0), m_hasTimeStamp(false), m_timeStampIndex(0)
{}

void BinLogParser::typeDescriptor::finalize(const timeStampType &timeStamp)
{
    int tempIndex = m_labels.indexOf(timeStamp.m_name);

    if(tempIndex != -1)
    {
        m_hasTimeStamp = true;
        m_timeStampIndex = tempIndex;
    }
}

void BinLogParser::typeDescriptor::addTimeStampField(const BinLogParser::timeStampType &timeStamp)
{
    // Add timestamp name to label list
    m_labels.push_front(timeStamp.m_name);
    // Add timestamp format code to format string
    m_format.prepend('Q');
    // and increase the length by 8 bytes ('Q' is a quint_64)
    m_length += 8;

    m_hasTimeStamp = true;
    m_timeStampIndex = 0;
}

void BinLogParser::typeDescriptor::replaceLabelName(const QString &oldName, const QString newName)
{
    int tempIndex = m_labels.indexOf(oldName);

    if(tempIndex != -1)
    {
        m_labels[tempIndex] = newName;
    }
}

QString BinLogParser::typeDescriptor::getLabelAtIndex(int index) const
{
    return index < m_labels.size() ? m_labels.at(index) : QString("NoLabel");
}

bool BinLogParser::typeDescriptor::hasNoTimestamp() const
{
    return !m_hasTimeStamp;
}

bool BinLogParser::typeDescriptor::isValid() const
{
    // Special handling for FMT messages as they are corrupt in some logs. This is not a real
    // problem as the FMT is parsed by a fixed scheme at the moment.
    if(m_ID == s_FMTMessageType)
    {
        if(m_format.size() != m_labels.size())
        {
            QLOG_WARN() << "typeDescriptor::valid() Corrupt FMT descriptor found - known bug in some logs - "
                        << "trying to ignore...";
        }
        return (m_ID != 0xFF) && (m_length > 0) && (m_name.size() > 0) &&
               (m_format.size() > 0) && (m_labels.size() > 0);
    }
    // STRT message has also a special behaviour as it has no data fields in older logs.
    else if(m_ID == s_STRTMessageType)
    {
        if(m_format.size() == 0 && m_length == 3)
        {
            QLOG_WARN() << "typeDescriptor::valid() Corrupt STRT descriptor found - known bug in some logs - "
                        << "trying to ignore...";
        }
        return (m_ID != 0xFF) && (m_length > 0) && (m_name.size() > 0) &&
               (m_format.size() == m_labels.size());
    }
    else
    {
        return (m_ID != 0xFF) && (m_length > 0) && (m_name.size() > 0) &&
               (m_format.size() > 0) && (m_format.size() == m_labels.size());
    }
}

//*****************************************

BinLogParser::BinLogParser(AP2DataPlot2DModel *model, IParserCallback *object) :
    m_callbackObject(object),
    m_dataModel(model),
    m_dataPos(0),
    m_messageType(0),
    m_MessageCounter(0),
    m_stop(false),
    m_loadedLogType(MAV_TYPE_GENERIC),
    m_timeErrorCount(0),
    m_lastValidTimeStamp(0)
{
    QLOG_DEBUG() << "BinLogParser::BinLogParser - CTOR";
    if(m_dataModel == 0)
    {
        QLOG_ERROR() << "BinLogParser::BinLogParser - No valid datamodel - parsing will not be possible";
    }
    if(m_callbackObject == 0)
    {
        QLOG_ERROR() << "BinLogParser::BinLogParser - No valid callback object - parsing will not be possible";
    }

    // flash logs can have different timestamps
    m_possibleTimestamps.push_back(timeStampType("TimeUS", 1000000.0));
    m_possibleTimestamps.push_back(timeStampType("TimeMS", 1000.0));
}

BinLogParser::~BinLogParser()
{
    QLOG_DEBUG() << "BinLogParser::BinLogParser - DTOR";
}

AP2DataPlotStatus BinLogParser::parse(QFile &logfile)
{
    QLOG_DEBUG() << "BinLogParser::parse:" << logfile.fileName();

    if(!m_dataModel || !m_callbackObject)
    {
        QLOG_ERROR() << "BinLogParser::BinLogParser - No valid datamodel or callback object - parsing stopped";
        return m_logLoadingState;
    }

    if(!m_dataModel->startTransaction())
    {
        m_callbackObject->onError(m_dataModel->getError());
        return m_logLoadingState;
    }

    int noMessageBytes = 0;

    while(!logfile.atEnd() && !m_stop)
    {
        m_callbackObject->onProgress(logfile.pos(),logfile.size());
        // remove no message bytes from data block
        if(m_dataPos > s_MinHeaderSize)
        {
            m_dataBlock.remove(0, m_dataPos - s_MinHeaderSize);
        }
        m_dataBlock.reserve(8320);
        m_dataBlock.append(logfile.read(8192));
        m_dataPos = 0;

        while(((m_dataBlock.size() - m_dataPos) > s_MinHeaderSize) && !m_stop)
        {
            if(!headerIsValid()) // checks the header and sets m_messageType
            {
                noMessageBytes++;
                continue;
            }

            if(m_messageType == s_FMTMessageType)
            {
                typeDescriptor descriptor;
                if(parseFMTMessage(descriptor))
                {
                    if(descriptor.m_name == "GPS")
                    {
                        // Special handling for "GPS" messages that have a "TimeMS"
                        // timestamp but scaling and value does not mach all other time stamps
                        descriptor.replaceLabelName("TimeMS", "GPSTimeMS");
                    }
                    if(m_activeTimestamp.valid())
                    {
                        descriptor.finalize(m_activeTimestamp);
                        if(!extendedStoreDescriptor(descriptor))
                        {
                            return m_logLoadingState;
                        }
                    }
                    else
                    {
                        checkForValidTimestamp(descriptor);
                    }
                }
                else
                {
                    break;  // not enough data break the inner loop to fetch some more...
                }
            }
            // Data packet
            else if(m_typeToDescriptorMap.contains(m_messageType))
            {
                QList<NameValuePair> NameValuePairList;
                typeDescriptor descriptor = m_typeToDescriptorMap.value(m_messageType);
                if(parseDataByDescriptor(NameValuePairList, descriptor))
                {
                    if(NameValuePairList.size() >= 1)   // need at least one element
                    {
                        if(!storeNameValuePairList(NameValuePairList, descriptor))
                        {
                            return m_logLoadingState;
                        }
                        if((m_loadedLogType == MAV_TYPE_GENERIC) && (descriptor.m_name == "PARM"))
                        {
                            detectMavType(NameValuePairList);
                        }
                    }
                    else
                    {
                        m_logLoadingState.corruptDataRead(static_cast<int>(m_MessageCounter),
                                                          "No values within data message");
                    }
                }
                else
                {
                    break;  // not enough data break the inner loop to fetch some more...
                }
            }
            else
            {
                m_logLoadingState.corruptDataRead(static_cast<int>(m_MessageCounter),
                                                  "Read data without having a valid format descriptor - "
                                                  "Message type is " + QString::number(m_messageType));
            }
        }
    }
    if (noMessageBytes > 0)
    {
        QLOG_DEBUG() << "BinLogParser::parse(): Non packet bytes found in log file. " << noMessageBytes << " bytes filtered out. This may be a corrupt log";
        m_logLoadingState.setNoMessageBytes(noMessageBytes);
    }

    if (!m_dataModel->endTransaction())
    {
        m_callbackObject->onError(m_dataModel->getError());
        return m_logLoadingState;
    }
    m_dataModel->setAllRowsHaveTime(true, m_activeTimestamp.m_name, m_activeTimestamp.m_divisor);

    return m_logLoadingState;
}

void BinLogParser::stopParsing()
{
    QLOG_DEBUG() << "BinLogParser::stopParsing";
    m_stop = true;
}

bool BinLogParser::headerIsValid()
{
    if((static_cast<quint8>(m_dataBlock.at(m_dataPos++)) == s_StartByte1) &&
       (static_cast<quint8>(m_dataBlock.at(m_dataPos++)) == s_StartByte2))
    {
        m_messageType = static_cast<quint8>(m_dataBlock.at(m_dataPos++));
        return true;
    }
    m_messageType = 0;
    return false;
}

bool BinLogParser::parseFMTMessage(typeDescriptor &desc)
{
    desc.m_ID     = static_cast<quint8>(m_dataBlock.at(m_dataPos++));
    desc.m_length = static_cast<quint8>(m_dataBlock.at(m_dataPos++));
    if((m_dataBlock.size() - m_dataPos) < (desc.m_length - s_HeaderOffset - 2)) // the -2 is for the 2 increments made in lines above
    {
        return false;   // do not have enough data to parse the packet
    }

    desc.m_name = m_dataBlock.mid(m_dataPos, s_FMTNameSize);
    m_dataPos += s_FMTNameSize;
    desc.m_format = m_dataBlock.mid(m_dataPos, s_FMTFormatSize);
    m_dataPos += s_FMTFormatSize;
    QString tmpStr = m_dataBlock.mid(m_dataPos, s_FMTLabelsSize);
    if(tmpStr.size() > 0)
    {
        desc.m_labels = tmpStr.split(",");
    }
    m_dataPos += s_FMTLabelsSize;

    m_dataBlock.remove(0, m_dataPos);
    m_dataPos = 0;
    return true;
}

bool BinLogParser::storeDescriptor(typeDescriptor desc)
{
    if(desc.isValid())
    {
        if(!m_typeToDescriptorMap.contains(desc.m_ID))
        {
            m_typeToDescriptorMap.insert(desc.m_ID, desc);

            if(desc.m_ID != s_FMTMessageType)   // the descriptor for the FMT message itself shall not be stored in DB
            {
                if(desc.hasNoTimestamp())
                {
                    desc.addTimeStampField(m_activeTimestamp);
                }

                if (!m_dataModel->addType(desc.m_name, desc.m_ID, desc.m_length, desc.m_format, desc.m_labels))
                {
                    QString currentError = m_dataModel->getError();
                    m_dataModel->endTransaction(); //endTransaction can re-set the error if it errors, but we should try it anyway.
                    m_callbackObject->onError(currentError);
                    return false;
                }
                m_MessageCounter++;
            }
        }
        else
        {
           QLOG_WARN() << "BinLogParser::storeDescriptor(): m_typeToDescriptorMap already contains descriptor with ID "
                       << desc.m_ID << " ignoring the new one";
           m_logLoadingState.corruptFMTRead(static_cast<int>(m_MessageCounter), desc.m_name + " format data: Doubled entry found. Using the first one.");
        }
    }
    else
    {
        QLOG_WARN() << "BinLogParser::storeDescriptor(): Invalid type descriptor found for type" << desc.m_ID << ":" << desc.m_name;
        m_logLoadingState.corruptFMTRead(static_cast<int>(m_MessageCounter), desc.m_name + " format data: Corrupt or missing. Message type is:0x" +
                                       QString::number(desc.m_ID, 16));
    }
    return true;
}

bool BinLogParser::parseDataByDescriptor(QList<NameValuePair> &NameValuePairList, const typeDescriptor &desc)
{

    if((m_dataBlock.size() - m_dataPos) < (desc.m_length - s_HeaderOffset))
    {
        return false;
    }

    QByteArray data = m_dataBlock.mid(m_dataPos, (desc.m_length - s_HeaderOffset));
    QDataStream packetstream(data);
    packetstream.setByteOrder(QDataStream::LittleEndian);
    packetstream.setFloatingPointPrecision(QDataStream::SinglePrecision);

    NameValuePairList.clear();

    for (int i = 0; i < desc.m_format.size(); i++)
    {
        QChar typeCode = desc.m_format.at(i);
        if (typeCode == 'b') //int8_t
        {
            qint8 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'B') //uint8_t
        {
            quint8 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'h') //int16_t
        {
            qint16 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'H') //uint16_t
        {
            quint16 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'i') //int32_t
        {
            qint32 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'I') //uint32_t
        {
            quint32 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'f') //float
        {
            float val;
            packetstream >> val;
            if (val != val) // This tests for not a number
            {
                QLOG_WARN() << "Corrupted log data found - Graphing may not work as expected for data of type " << desc.m_name;
                m_logLoadingState.corruptDataRead(static_cast<int>(m_MessageCounter), "Corrupt data element found when decoding " + desc.m_name + " data.");
                NameValuePairList.clear();
                break;
            }
            else
            {
                NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
            }
        }
        else if (typeCode == 'n') //char(4)
        {

            QString val;
            for (int j = 0; j < 4; j++)
            {
                quint8 ch;
                packetstream >> ch;
                if(ch)
                {
                    val.append(ch);
                }
            }
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'N') //char(16)
        {
            QString val;
            for (int j = 0; j < 16; j++)
            {
                quint8 ch;
                packetstream >> ch;
                if(ch)
                {
                    val.append(ch);
                }
            }
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'Z') //char(64)
        {
            QString val;
            for (int j = 0; j < 64; j++)
            {
                quint8 ch;
                packetstream >> ch;
                if(ch)
                {
                    val.append(ch);
                }
            }
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'c') //int16_t * 100
        {
            qint16 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val / 100.0));
        }
        else if (typeCode == 'C') //uint16_t * 100
        {
            quint16 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val / 100.0));
        }
        else if (typeCode == 'e') //int32_t * 100
        {
            qint32 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val / 100.0));
        }
        else if (typeCode == 'E') //uint32_t * 100
        {
            quint32 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val / 100.0));
        }
        else if (typeCode == 'L') //uint32_t GPS Lon/Lat * 10000000
        {
            qint32 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val / 10000000.0));
        }
        else if (typeCode == 'M')
        {
            qint8 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'q')
        {
            qint64 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else if (typeCode == 'Q')
        {
            quint64 val;
            packetstream >> val;
            NameValuePairList.append(NameValuePair(desc.getLabelAtIndex(i), val));
        }
        else
        {
            //Unknown!
            QLOG_DEBUG() << "BinLogParser::extractByDescriptor(): ERROR UNKNOWN DATA TYPE " << typeCode;
            m_logLoadingState.corruptDataRead(static_cast<int>(m_MessageCounter), "Unknown data type: " + QString(typeCode) + " when decoding " + desc.m_name);
            NameValuePairList.clear();
            break;
        }
    }

    m_dataBlock.remove(0, desc.m_length + m_dataPos - s_HeaderOffset);
    m_dataPos = 0;

    return true;
}

bool BinLogParser::storeNameValuePairList(QList<NameValuePair> &NameValuePairList, const typeDescriptor &desc)
{
    if(desc.hasNoTimestamp())
    {
        NameValuePairList.prepend(NameValuePair(m_activeTimestamp.m_name, m_lastValidTimeStamp));
    }
    else
    {
        readTimeStamp(NameValuePairList, desc.m_timeStampIndex);
    }

    if (!m_dataModel->addRow(desc.m_name, NameValuePairList, m_activeTimestamp.m_name))
    {
        QString currentError = m_dataModel->getError();
        m_dataModel->endTransaction(); //endTransaction can re-set the error if it errors, but we should try it anyway.
        m_callbackObject->onError(currentError);
        return false;
    }
    m_MessageCounter++;
    m_logLoadingState.validDataRead();
    return true;
}


void BinLogParser::checkForValidTimestamp(typeDescriptor &desc)
{
    foreach(const timeStampType &timeStamp, m_possibleTimestamps)
    {
        if (desc.m_labels.contains(timeStamp.m_name))
        {
            m_activeTimestamp = timeStamp;
            break;
        }
    }
    if(m_activeTimestamp.valid())
    {
        desc.finalize(m_activeTimestamp);
    }
    m_descriptorForDeferredStorage.push_back(desc);
}


bool BinLogParser::extendedStoreDescriptor(const typeDescriptor &desc)
{
    bool rc = true;
    foreach (const typeDescriptor &descriptor, m_descriptorForDeferredStorage)
    {
        bool localRc = storeDescriptor(descriptor);
        rc = rc && localRc;
    }
    m_descriptorForDeferredStorage.clear();
    if (rc)
    {
        rc = storeDescriptor(desc);
    }
    return rc;
}


void BinLogParser::readTimeStamp(QList<NameValuePair> &valuepairlist, const int timeStampIndex)
{

    quint64 tempVal = static_cast<quint64>(valuepairlist.at(timeStampIndex).second.toULongLong());
    // check if time is increasing
    if (tempVal >= m_lastValidTimeStamp)
    {
        m_lastValidTimeStamp = tempVal;
    }
    else
    {
        if(m_timeErrorCount < 50)
        {
            QLOG_WARN() << "Corrupt data read: Time is not increasing! Last valid time stamp:"
                         << QString::number(m_lastValidTimeStamp) << " actual read time stamp is:"
                         << QString::number(tempVal);
            ++m_timeErrorCount;
        }
        else if(m_timeErrorCount < 51)
        {
            QLOG_WARN() << "Supressing further time is not increasing messages....";
            ++m_timeErrorCount;
        }
        m_logLoadingState.corruptTimeRead(m_MessageCounter, "Log time is not increasing! Last Time:" +
                                          QString::number(m_lastValidTimeStamp) + " new Time:" +
                                          QString::number(tempVal));
        // if not increasing set to last valid value
        valuepairlist[timeStampIndex].second = m_lastValidTimeStamp;
    }
}


void BinLogParser::detectMavType(const QList<NameValuePair> &valuepairlist)
{
    // Name field is not always on same Index. So first search for the right position...
    int nameIndex = 0;
    for (int i = 0; i < valuepairlist.size(); ++i)
    {
        if (valuepairlist[i].first == "Name")
        {
            nameIndex = i;
            break;
        }
    }
    //...and then use it to check the values.
    if (valuepairlist[nameIndex].second == "RATE_RLL_P" || valuepairlist[nameIndex].second == "H_SWASH_PLATE"
            || valuepairlist[nameIndex].second == "ATC_RAT_RLL_P" ) // ATC_RAT_RLL_P Used in AC3.4+
    {
        m_loadedLogType = MAV_TYPE_QUADROTOR;
    }
    else if (valuepairlist[nameIndex].second == "PTCH2SRV_P")
    {
        m_loadedLogType = MAV_TYPE_FIXED_WING;
    }
    else if (valuepairlist[nameIndex].second == "SKID_STEER_OUT")
    {
        m_loadedLogType = MAV_TYPE_GROUND_ROVER;
    }

    if(m_loadedLogType != MAV_TYPE_GENERIC)
    {
        m_logLoadingState.setMavType(m_loadedLogType);
    }
}