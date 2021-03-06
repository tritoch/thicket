#include "ServerRoom.h"

#include <stdlib.h>
#include <memory>

#include <QTimer>

#include "ClientConnection.h"
#include "HumanPlayer.h"
#include "BotPlayer.h"
#include "StupidBotPlayer.h"

static const int CREATED_ROOM_EXPIRATION_SECONDS   =  10;
static const int ABANDONED_ROOM_EXPIRATION_SECONDS = 120;

ServerRoom::ServerRoom( unsigned int                      roomId,
                        const std::string&                password,
                        const proto::RoomConfig&          roomConfig,
                        const DraftCardDispenserSharedPtrVector<DraftCard>& dispensers,
                        const Logging::Config&            loggingConfig,
                        QObject*                          parent )
:   QObject( parent ),
    mRoomId( roomId ),
    mPassword( password ),
    mRoomConfig( roomConfig ),
    mDispensers( dispensers ),
    mChairCount( mRoomConfig.draft_config().chair_count() ),
    mBotPlayerCount( mRoomConfig.bot_count() ),
    mDraftComplete( false ),
    mPublicStatePresent( false ),
    mPostRoundTimerActive( false ),
    mPostRoundTimerTicksRemaining( 0 ),
    mLoggingConfig( loggingConfig ),
    mLogger( mLoggingConfig.createLogger() )
{
    // Delay initialization so that this object's signals can be connected.
    // This allows the "expired" and bot "player count changed" signals to
    // be properly connected and sent/received.
    QTimer::singleShot( 0, this, SLOT(initialize()) );
}


void
ServerRoom::initialize()
{
    if( (mChairCount <= 0) )
    {
        mLogger->error( "invalid room configuration!" );
        emit roomExpired();
        return;
    }

    for( unsigned int i = 0; i < mChairCount; ++i )
    {
        mPlayerList.append( 0 );
        mChairStateList.append( CHAIR_STATE_EMPTY );
    }

    mDraftPtr = new DraftType( mRoomConfig.draft_config(), mDispensers );
    mDraftPtr->addObserver( this );

    mRoomExpirationTimer = new QTimer( this );
    connect( mRoomExpirationTimer, &QTimer::timeout, this, &ServerRoom::roomExpired );

    // Start the room expiration timer immediately.  Normally the creating
    // client will join it immediately, but if that doesn't happen the room
    // needs to be cleaned up.
    mRoomExpirationTimer->start( CREATED_ROOM_EXPIRATION_SECONDS * 1000 );

    mDraftTimer = new QTimer( this );
    connect(mDraftTimer, SIGNAL(timeout()), this, SLOT(handleDraftTimerTick()));

    // Add in the bots.
    unsigned int botPlayerCount = mBotPlayerCount;
    if( botPlayerCount > mChairCount )
    {
        mLogger->warn( "more bots than chairs!" );
        botPlayerCount = mChairCount;
    }
    Logging::Config stupidBotLoggingConfig = mLoggingConfig.createChildConfig( "stupidbot" );

    unsigned int chairIndex = 0;
    for( unsigned int i = 0; i < botPlayerCount; ++i )
    {
        mLogger->debug( "Placing bot in chair {}", chairIndex );
        BotPlayer* bot = new StupidBotPlayer( chairIndex, stupidBotLoggingConfig );
        mBotList.append( bot );
        mPlayerList[chairIndex] = bot;
        mChairStateList[ chairIndex ] = CHAIR_STATE_READY;

        // The bot must observe the draft to get the observation callbacks.
        mDraftPtr->addObserver( bot );

        // Slot the bots into every other chair to be as fair as possible
        // to the real players.
        chairIndex += 2;
        if( chairIndex >= mChairCount )
        {
            // Wrap back around starting with chair 1.
            chairIndex = 1;
        }

    }

    if( botPlayerCount > 0 )
    {
        emit playerCountChanged( getPlayerCount() );
    }
}


ServerRoom::~ServerRoom()
{
    mLogger->trace( "~ServerRoom" );
    for (int i = 0; i < mBotList.size(); ++i)
    {
        delete mBotList.at(i);
    }
    for (int i = 0; i < mHumanList.size(); ++i)
    {
        delete mHumanList.at(i);
    }
    delete mDraftPtr;
}


bool
ServerRoom::join( ClientConnection* clientConnection, const std::string& name, const std::string& password, int& chairIndex )
{
    // REFACTOR - this code and rejoin() have a LOT in common

    // This could be a rejoin - find the human by name.
    HumanPlayer* humanPlayer = getHumanPlayer( name );
    if( humanPlayer  )
    {
        mLogger->debug( "join: rejoining existing player to room" );
        return rejoin( clientConnection, name );
    }

    if( !mPassword.empty() && (password != mPassword) )
    {
        sendJoinRoomFailureRsp( clientConnection, proto::JoinRoomFailureRsp::RESULT_INVALID_PASSWORD, mRoomId );
        return false;
    }

    int playerIdx = getNextAvailablePlayerIndex();
    if( playerIdx == -1 )
    {
        sendJoinRoomFailureRsp( clientConnection, proto::JoinRoomFailureRsp::RESULT_ROOM_FULL, mRoomId );
        return false;
    }
    chairIndex = playerIdx;

    HumanPlayer *human = new HumanPlayer( playerIdx, mDraftPtr, mLoggingConfig.createChildConfig( "humanplayer" ), this );
    connect( human, &HumanPlayer::readyUpdate, this, &ServerRoom::handleHumanReadyUpdate );
    connect( human, &HumanPlayer::deckUpdate, this, &ServerRoom::handleHumanDeckUpdate );
    human->setName( name );
    human->setClientConnection( clientConnection );
    mClientConnectionMap.insert( clientConnection, human );
    mHumanList.append( human );
    mPlayerList[playerIdx] = human;
    mChairStateList[playerIdx] = CHAIR_STATE_STANDBY;
    mLogger->debug( "joined human {} with connection {} to player map at index {}:",
            (std::size_t)human, (std::size_t)clientConnection, playerIdx );
    for( int i = 0; i < mPlayerList.count(); ++i )
    {
        mLogger->debug( "   {}", (std::size_t)(mPlayerList[i]) );
    }

    // With at least one connection don't let the room expire.
    mRoomExpirationTimer->stop();

    // The human must observe the draft to get the observation callbacks.
    mDraftPtr->addObserver( human );

    // Inform the client that the room join was successful.
    sendJoinRoomSuccessRspInd( clientConnection, mRoomId, false, chairIndex );

    emit playerCountChanged( getPlayerCount() );

    // Inform all client connections of the room occupants changes.
    broadcastRoomOccupantsInfo();

    return true;
}


void
ServerRoom::leave( ClientConnection* clientConnection )
{
    QMap<ClientConnection*,HumanPlayer*>::iterator iter = mClientConnectionMap.find( clientConnection );
    if( iter != mClientConnectionMap.end() )
    {
        HumanPlayer* human = iter.value();
        const int chairIndex = human->getChairIndex();

        human->removeClientConnection();
        mClientConnectionMap.remove( clientConnection );
        mLogger->debug( "removed client {} from map", (std::size_t)clientConnection );

        if( mChairStateList[chairIndex] == CHAIR_STATE_ACTIVE )
        {
            // We removed the connection above, but leave the human in the
            // human list and maintain state in case the connection is
            // re-established.
            mChairStateList[chairIndex] = CHAIR_STATE_DEPARTED;

            // Draft was started so if the room now has no more connectons
            // start the room expiration timer.
            if( mClientConnectionMap.isEmpty() )
            {
                mLogger->debug( "starting room expiration timer" );
                mRoomExpirationTimer->start( ABANDONED_ROOM_EXPIRATION_SECONDS * 1000 );
            }
        }
        else
        {
            // The draft hasn't started yet, so remove and destroy the human.

            mDraftPtr->removeObserver( human );
            mHumanList.removeOne( human );
            delete human;

            // Null out player list entry and mark chair state as empty
            mPlayerList[chairIndex] = 0;
            mChairStateList[chairIndex] = CHAIR_STATE_EMPTY;

            // Draft hasn't started, so if the room now has no more
            // connections treat it as expired.
            if( mClientConnectionMap.isEmpty() )
            {
                emit roomExpired();
            }
        }

        emit playerCountChanged( getPlayerCount() );

        // Inform all client connections of the room occupants changes.
        broadcastRoomOccupantsInfo();
    }
    else
    {
        mLogger->warn( "unknown client disconnect" );
    }
}


bool
ServerRoom::rejoin( ClientConnection* clientConnection, const std::string& name )
{
    mLogger->trace( "rejoin room, client={}, name={}", (std::size_t)clientConnection, name );

    // Find the human by name.
    HumanPlayer* humanPlayer = getHumanPlayer( name );
    if( humanPlayer == nullptr )
    {
        mLogger->warn( "rejoin room error: player not found" );
        return false;
    }

    // Make sure the human was actually disconnected.
    const int chairIndex = humanPlayer->getChairIndex();
    if( mChairStateList[chairIndex] != CHAIR_STATE_DEPARTED )
    {
        mLogger->warn( "rejoin room error: player not disconnected" );
        return false;
    }

    // Update internals.
    humanPlayer->setClientConnection( clientConnection );
    mClientConnectionMap.insert( clientConnection, humanPlayer );
    mChairStateList[chairIndex] = CHAIR_STATE_ACTIVE;
    mLogger->debug( "rejoined human {} with connection {} to player map at index {}:",
            (std::size_t)humanPlayer, (std::size_t)clientConnection, chairIndex );
    for( int i = 0; i < mPlayerList.count(); ++i )
    {
        mLogger->debug( "  {}", (std::size_t)(mPlayerList[i]) );
    }

    // With at least one connection don't let the room expire.
    mRoomExpirationTimer->stop();

    // Send the user a room join success indication with the rejoin flag set.
    sendJoinRoomSuccessRspInd( clientConnection, mRoomId, true, chairIndex );

    emit playerCountChanged( getPlayerCount() );

    // Inform all occupants (including the new user) of user states.
    broadcastRoomOccupantsInfo();

    // Send the rejoining user their inventory of cards.
    humanPlayer->sendInventoryToClient();

    // Send user a room stage update indication.
    proto::ServerToClientMsg msg;
    proto::RoomStageInd* roomStageInd = msg.mutable_room_stage_ind();
    switch( mDraftPtr->getState() )
    {
        case DraftType::STATE_NEW:
            roomStageInd->set_stage( proto::RoomStageInd::STAGE_NEW );
            break;
        case DraftType::STATE_RUNNING:
            {
                roomStageInd->set_stage( proto::RoomStageInd::STAGE_RUNNING );
                proto::RoomStageInd::RoundInfo* roundInfo = roomStageInd->mutable_round_info();
                roundInfo->set_round( mDraftPtr->getCurrentRound() );
                if( mPostRoundTimerActive )
                {
                    const int millis = getPostRoundTimeRemainingMillis();
                    mLogger->debug( "RoomStageInd: post-round timer active, setting to {}", millis );
                    roundInfo->set_post_round_time_remaining_millis( millis );
                }
            }
            break;
        case DraftType::STATE_COMPLETE:
            roomStageInd->set_stage( proto::RoomStageInd::STAGE_COMPLETE );
            break;
        default:
            mLogger->error( "unhandled room state {}", mDraftPtr->getState() );
    }
    mLogger->debug( "sending RoomStageInd, size={} to client {}",
            msg.ByteSize(), (std::size_t)clientConnection );
    clientConnection->sendProtoMsg( msg );

    // Send current draft state information if draft is running.
    if( mDraftPtr->getState() == DraftType::STATE_RUNNING )
    {
        // Send user current pack, if any.
        humanPlayer->sendCurrentPackToClient();

        // Update the client's public state, if any.
        sendPublicState( { clientConnection } );
    }

    // Send all current hashes if the round is complete.
    if( mDraftPtr->getState() == DraftType::STATE_COMPLETE )
    {
        msg.Clear();
        proto::RoomChairsDeckInfoInd* deckInfoInd = msg.mutable_room_chairs_deck_info_ind();

        for( auto human : mHumanList )
        {
            proto::RoomChairsDeckInfoInd::Chair* chair = deckInfoInd->add_chairs();
            chair->set_chair_index( human->getChairIndex() );
            chair->set_cockatrice_hash( human->getCockatriceHash().toStdString() );
            chair->set_mws_hash( "" );
        }

        mLogger->debug( "sending deckInfoInd, size={} to client {}",
                msg.ByteSize(), (std::size_t)clientConnection );
        clientConnection->sendProtoMsg( msg );
    }

    return true;
}


bool
ServerRoom::getChairInfo( unsigned int  chairIndex,
                          std::string&  name,           // output
                          bool&         isBot,          // output
                          ChairState&   state,          // output
                          unsigned int& packsQueued,    // output
                          unsigned int& ticksRemaining  /* output */ ) const
{
    if( chairIndex < mChairCount )
    {
        Player* player = mPlayerList[chairIndex];
        state = mChairStateList[chairIndex];
        if( player != nullptr )
        {
            name = player->getName();
            isBot = mBotList.contains( (BotPlayer*)player );
            packsQueued = mDraftPtr->getPackQueueSize( chairIndex );
            ticksRemaining = mDraftPtr->getTicksRemaining( chairIndex );
        }
        else
        {
            name.clear();
            isBot = false;
            packsQueued = 0;
            ticksRemaining = 0;
        }
        return true;
    }
    else
    {
        return false;
    }
}


void
ServerRoom::sendJoinRoomSuccessRspInd( ClientConnection* clientConnection,
                                       int               roomId,
                                       bool              rejoin,
                                       int               chairIndex )
{
    mLogger->trace( "sendJoinRoomSuccessRspInd" );
    proto::ServerToClientMsg msg;
    proto::JoinRoomSuccessRspInd* joinRoomSuccessRspInd = msg.mutable_join_room_success_rspind();
    joinRoomSuccessRspInd->set_room_id( roomId );
    joinRoomSuccessRspInd->set_rejoin( rejoin );
    joinRoomSuccessRspInd->set_chair_idx( chairIndex );

    // Assemble room configuration.
    proto::RoomConfig* roomConfig = joinRoomSuccessRspInd->mutable_room_config();
    *roomConfig = mRoomConfig;

    clientConnection->sendProtoMsg( msg );
}


void
ServerRoom::sendJoinRoomFailureRsp( ClientConnection* clientConnection, proto::JoinRoomFailureRsp_ResultType result, int roomId )
{
    mLogger->trace( "sendJoinRoomFailureRsp, result={}, roomId={}", result, roomId );
    proto::ServerToClientMsg msg;
    proto::JoinRoomFailureRsp* joinRoomFailureRsp = msg.mutable_join_room_failure_rsp();
    joinRoomFailureRsp->set_result( result );
    joinRoomFailureRsp->set_room_id( roomId );
    clientConnection->sendProtoMsg( msg );
}


void
ServerRoom::broadcastRoomOccupantsInfo()
{
    mLogger->trace( "broadcastRoomOccupantsInfo" );

    // Assemble the message.
    proto::ServerToClientMsg msg;
    proto::RoomOccupantsInfoInd* roomOccupantsInfoInd = msg.mutable_room_occupants_info_ind();
    roomOccupantsInfoInd->set_room_id( mRoomId );

    for( int i = 0; i < mPlayerList.count(); ++i )
    {
        if( mPlayerList[i] != 0 )
        {
            proto::RoomOccupantsInfoInd::Player* player = roomOccupantsInfoInd->add_players();
            player->set_chair_index( i );
            player->set_name( mPlayerList[i]->getName() );
            bool isBot = mBotList.contains( (BotPlayer*) mPlayerList[i] );
            player->set_is_bot( isBot );
            switch( mChairStateList[i] )
            {
                case CHAIR_STATE_STANDBY:  player->set_state( proto::RoomOccupantsInfoInd::Player::STATE_STANDBY  ); break;
                case CHAIR_STATE_READY:    player->set_state( proto::RoomOccupantsInfoInd::Player::STATE_READY    ); break;
                case CHAIR_STATE_ACTIVE:   player->set_state( proto::RoomOccupantsInfoInd::Player::STATE_ACTIVE   ); break;
                case CHAIR_STATE_DEPARTED: player->set_state( proto::RoomOccupantsInfoInd::Player::STATE_DEPARTED ); break;
                default:                   mLogger->error( "unexpected chair state!" );
            }
        }
    }

    const int protoSize = msg.ByteSize();

    // Send the message to each client connection.
    for( auto clientConn : mClientConnectionMap.keys())
    {
        mLogger->debug( "sending roomOccupantsInfoInd, size={} to client {}",
                protoSize, (std::size_t)clientConn );
        clientConn->sendProtoMsg( msg );
    }
}


void
ServerRoom::broadcastBoosterDraftState()
{
    // OPTIMIZATION - could cache the outgoing message to cut down on
    // redundant messages going out.  Pack queue size changes create at least
    // two redundant message per event.

    // Build the message.
    proto::ServerToClientMsg msg;
    proto::BoosterDraftStateInd* ind = msg.mutable_booster_draft_state_ind();
    ind->set_millis_until_next_sec( mDraftTimer->remainingTime() );

    for( int i = 0; i < mDraftPtr->getChairCount(); ++i )
    {
        proto::BoosterDraftStateInd::Chair* chair = ind->add_chairs();
        chair->set_chair_index( i );
        chair->set_queued_packs( mDraftPtr->getPackQueueSize( i ) );
        chair->set_time_remaining( mDraftPtr->getTicksRemaining( i ) );
    }

    const int protoSize = msg.ByteSize();

    // Send the message to all active client connections.
    for( ClientConnection* clientConnection : mClientConnectionMap.keys() )
    {
        mLogger->debug( "sending roomChairsInfoInd, size={} to client {}",
                protoSize, (std::size_t)clientConnection );
        clientConnection->sendProtoMsg( msg );
    }
}


void
ServerRoom::sendPublicState( const QList<ClientConnection*> clientConnections )
{
    if( !mPublicStatePresent )
    {
        mLogger->info( "public state not present, not sending PublicStateInd" );
        return;
    }

    proto::ServerToClientMsg msg;
    proto::PublicStateInd* publicStateInd = msg.mutable_public_state_ind();

    publicStateInd->set_pack_id( mPublicPackId );
    for( const auto& state : mPublicCardStates )
    {
        proto::PublicStateInd::CardState* cardState = publicStateInd->add_card_states();
        proto::Card* card = cardState->mutable_card();
        card->set_name( state.getCard().name );
        card->set_set_code( state.getCard().setCode );
        cardState->set_selected_chair_index( state.getSelectedChairIndex() );
        cardState->set_selected_order( state.getSelectedOrder() );
    }
    publicStateInd->set_active_chair_index( mPublicActiveChairIndex );

    publicStateInd->set_time_remaining_secs( mDraftPtr->getTicksRemaining( mPublicActiveChairIndex ) );
    publicStateInd->set_millis_until_next_sec( mDraftTimer->remainingTime() );

    const int protoSize = msg.ByteSize();

    // Send the message to all active client connections.
    for( ClientConnection* clientConnection : clientConnections )
    {
        mLogger->debug( "sending PublicStateInd, size={} to client {}",
                protoSize, (std::size_t)clientConnection );
        clientConnection->sendProtoMsg( msg );
    }
}


void
ServerRoom::broadcastRoomChairsDeckInfo( const HumanPlayer& human )
{
    // Build the message.
    proto::ServerToClientMsg msg;
    proto::RoomChairsDeckInfoInd* ind = msg.mutable_room_chairs_deck_info_ind();

    proto::RoomChairsDeckInfoInd::Chair* chair = ind->add_chairs();
    chair->set_chair_index( human.getChairIndex() );
    chair->set_cockatrice_hash( human.getCockatriceHash().toStdString() );
    chair->set_mws_hash( "" );

    const int protoSize = msg.ByteSize();

    // Send the message to all active client connections.
    for( ClientConnection* clientConnection : mClientConnectionMap.keys() )
    {
        mLogger->debug( "sending roomChairsDeckInfoInd, size={} to client {}",
                protoSize, (std::size_t)clientConnection );
        clientConnection->sendProtoMsg( msg );
    }
}


void
ServerRoom::handleDraftTimerTick()
{
    mLogger->trace( "tick" );

    if( mPostRoundTimerActive ) mPostRoundTimerTicksRemaining--;

    mDraftPtr->tick();

    if( (mDraftPtr->getState() == DraftType::STATE_RUNNING) && (mDraftPtr->isBoosterRound()) )
    {
        broadcastBoosterDraftState();
    }
}


void
ServerRoom::handleHumanReadyUpdate( bool ready )
{
    mLogger->trace( "handleHumanReady: ready={}", ready );
    HumanPlayer *human = qobject_cast<HumanPlayer*>( QObject::sender() );

    bool allChairsReady = false;

    const int chairIndex = human->getChairIndex();
    ChairState& state = mChairStateList[chairIndex];

    if( (state == CHAIR_STATE_READY) && !ready )
    {
        mLogger->debug( "human at chair {} moving to STANDBY", chairIndex );
        state = CHAIR_STATE_STANDBY;
    }
    else if( (state == CHAIR_STATE_STANDBY) && ready )
    {
        mLogger->debug( "human at chair {} moving to READY", chairIndex );
        state = CHAIR_STATE_READY;

        allChairsReady = true;
        for( int i = 0; i < mChairStateList.count(); ++i )
        {
            if( mChairStateList[i] != CHAIR_STATE_READY )
            {
                allChairsReady = false;
                break;
            }
        }

        // If all chairs are ready, move to active; draft will start
        if( allChairsReady )
        {
            mLogger->debug( "all slots full and ready, moving all chairs to ACTIVE" );
            for( int i = 0; i < mChairStateList.count(); ++i )
            {
                mChairStateList[i] = CHAIR_STATE_ACTIVE;
            }
        }
    }

    // Broadcast new room occupants state info to all.
    broadcastRoomOccupantsInfo();

    // If all chairs are ready, let's go!
    if( allChairsReady )
    {
        mLogger->info( "starting the draft!" );
        mDraftTimer->start( 1000 );
        mDraftPtr->start();
    }
}


void
ServerRoom::handleHumanDeckUpdate()
{
    mLogger->trace( "handleHumanDeckUpdate" );

    // If the draft is complete, broadcast the deck update message.
    if( mDraftComplete )
    {
        HumanPlayer *human = qobject_cast<HumanPlayer*>( QObject::sender() );
        broadcastRoomChairsDeckInfo( *human );

        // OPTIMIZATION - this is going to send a lot of messages as
        // players manipulate decks.  Bundle updates an send out on a timer.
    }
}


void
ServerRoom::notifyPackQueueSizeChanged( DraftType& draft, int chairIndex, int packQueueSize )
{
    mLogger->trace( "chair {} packQueueSize={}", chairIndex, packQueueSize );

    broadcastBoosterDraftState();
}


void
ServerRoom::notifyPublicState( DraftType& draft, uint32_t packId, const std::vector<PublicCardState>& cardStates, int activeChairIndex )
{
    mLogger->trace( "public state update, activeChair={}", activeChairIndex );

    mPublicStatePresent = true;
    mPublicPackId = packId;
    mPublicCardStates = cardStates;
    mPublicActiveChairIndex = activeChairIndex;

    // Broadcast public state to all clients.
    sendPublicState( mClientConnectionMap.keys() );
}


void
ServerRoom::notifyPostRoundTimerStarted( DraftType& draft, int roundIndex, int ticksRemaining )
{
    mLogger->trace( "post-round timer started, roundIndex={} ticksRemaining={}", roundIndex, ticksRemaining );

    mPostRoundTimerActive = true;
    mPostRoundTimerTicksRemaining = ticksRemaining;

    if( ticksRemaining <= 0 )
    {
        mLogger->warn( "unexpected post-round timer ticksRemaining: {}", ticksRemaining );
        return;
    }

    const uint32_t postRoundTimeRemainingMillis = getPostRoundTimeRemainingMillis();

    // Send user a room stage update indication.
    proto::ServerToClientMsg msg;
    proto::RoomStageInd* roomStageInd = msg.mutable_room_stage_ind();
    roomStageInd->set_stage( proto::RoomStageInd::STAGE_RUNNING );
    proto::RoomStageInd::RoundInfo* roundInfo = roomStageInd->mutable_round_info();
    roundInfo->set_round( roundIndex );
    roundInfo->set_post_round_time_remaining_millis( postRoundTimeRemainingMillis );

    int protoSize = msg.ByteSize();
    mLogger->debug( "sending RoomStageInd (STAGE_RUNNING), size={} round={} postRoundTimerMillis={} ",
            protoSize, roomStageInd->round_info().round(), postRoundTimeRemainingMillis );

    // Send the message to all active client connections.
    for( ClientConnection* clientConnection : mClientConnectionMap.keys() )
    {
        mLogger->debug( "sending to client {}", (std::size_t)clientConnection );
        clientConnection->sendProtoMsg( msg );
    }
}


void
ServerRoom::notifyNewRound( DraftType& draft, int roundIndex )
{
    // Reset round-based flags.
    mPublicStatePresent = false;
    mPostRoundTimerActive = false;

    // Send user a room stage update indication.
    proto::ServerToClientMsg msg;
    proto::RoomStageInd* roomStageInd = msg.mutable_room_stage_ind();
    roomStageInd->set_stage( proto::RoomStageInd::STAGE_RUNNING );
    proto::RoomStageInd::RoundInfo* roundInfo = roomStageInd->mutable_round_info();
    roundInfo->set_round( roundIndex );

    int protoSize = msg.ByteSize();
    mLogger->debug( "sending RoomStageInd (STAGE_RUNNING), size={} round={}",
            protoSize, roomStageInd->round_info().round() );

    // Send the message to all active client connections.
    for( ClientConnection* clientConnection : mClientConnectionMap.keys() )
    {
        mLogger->debug( "sending to client {}", (std::size_t)clientConnection );
        clientConnection->sendProtoMsg( msg );
    }
}


void
ServerRoom::notifyDraftComplete( DraftType& draft )
{
    mLogger->debug( "draft complete, stopping timer" );
    mDraftTimer->stop();

    // This may not have been active, but safe to set false.
    mPostRoundTimerActive = false;

    mDraftComplete = true;

    // Send user a room stage update indication.
    proto::ServerToClientMsg msg;
    proto::RoomStageInd* roomStageInd = msg.mutable_room_stage_ind();
    roomStageInd->set_stage( proto::RoomStageInd::STAGE_COMPLETE );

    int protoSize = msg.ByteSize();
    mLogger->debug( "sending RoomStageInd (STAGE_COMPLETE), size={}", protoSize );

    // Send the message to all active client connections.
    for( ClientConnection* clientConnection : mClientConnectionMap.keys() )
    {
        mLogger->debug( "sending to client {}", (std::size_t)clientConnection );
        clientConnection->sendProtoMsg( msg );
    }

    // Send out all current hash values.
    // OPTIMIZATION - the message allows for multiple hashes, so this
    // could easily go out as a single message.
    for( auto human : mHumanList )
    {
        broadcastRoomChairsDeckInfo( *human );
    }
}


void
ServerRoom::notifyDraftError( DraftType& draft )
{
    // Send user a room error indication.
    proto::ServerToClientMsg msg;
    (void*) msg.mutable_room_error_ind();
    int protoSize = msg.ByteSize();
    mLogger->debug( "sending RoomErrorInd, size={}", protoSize );

    // Send the message to all active client connections.
    for( ClientConnection* clientConnection : mClientConnectionMap.keys() )
    {
        mLogger->debug( "sending to client {}", (std::size_t)clientConnection );
        clientConnection->sendProtoMsg( msg );
    }

    emit roomError();
}

int
ServerRoom::getNextAvailablePlayerIndex() const
{
    // Find the first NULL entry in the list.
    return mPlayerList.indexOf( 0 );
}


HumanPlayer*
ServerRoom::getHumanPlayer( const std::string& name ) const
{
    for( auto human : mHumanList )
    {
        if( human->getName() == name ) return human;
    }
    return nullptr;
}


int
ServerRoom::getPostRoundTimeRemainingMillis() const
{
    if( !mPostRoundTimerActive ) return -1;

    // Convert ticks to milliseconds and subtract off the 1-second timer value.
    return (mPostRoundTimerTicksRemaining * 1000) - (1000 - mDraftTimer->remainingTime());
}

