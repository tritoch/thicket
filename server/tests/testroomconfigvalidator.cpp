#include "catch.hpp"
#include "messages.pb.h"
#include "RoomConfigValidator.h"
#include "MtgJsonAllSetsData.h"

using namespace proto;

CATCH_TEST_CASE( "RoomConfigPrototype", "[roomconfigprototype]" )
{
    Logging::Config loggingConfig;
    loggingConfig.setName( "roomconfigvalidator" );
    loggingConfig.setStdoutLogging( true );
    loggingConfig.setLevel( spdlog::level::debug );

    const std::string allSetsDataFilename = "AllSets.json";
    FILE* allSetsDataFile = fopen( allSetsDataFilename.c_str(), "r" );
    if( allSetsDataFile == NULL )
        CATCH_FAIL( "Failed to open AllSets.json: it must be co-located with the test executable." );

    // Static to avoid reconstruction/parsing for every test case.
    static MtgJsonAllSetsData* allSets = nullptr;
    static auto allSetsSharedPtr = std::shared_ptr<const AllSetsData>( nullptr );
    if( !allSets )
    {
        allSets = new MtgJsonAllSetsData();
        allSetsSharedPtr = std::shared_ptr<const AllSetsData>( allSets );
        bool parseResult = allSets->parse( allSetsDataFile );
        fclose( allSetsDataFile );
        CATCH_REQUIRE( parseResult );
    }

    // Static to avoid reconstruction/parsing for every test case.
    static RoomConfigValidator roomConfigValidator( allSetsSharedPtr, loggingConfig );
    CreateRoomFailureRsp_ResultType failureResult;

    CATCH_SECTION( "Booster Rounds" )
    {

        //
        // Create a model RoomConfiguration that test cases can tweak.
        //

        const int CHAIR_COUNT = 8;
        RoomConfig roomConfig;
        roomConfig.set_name( "testroom" );
        roomConfig.set_password_protected( false );
        roomConfig.set_bot_count( 0 );

        DraftConfig* draftConfig = roomConfig.mutable_draft_config();
        draftConfig->set_chair_count( CHAIR_COUNT );

        // Currently this is hardcoded for three booster rounds.
        for( int i = 0; i < 3; ++i )
        {
            DraftConfig::CardDispenser* dispenser = draftConfig->add_dispensers();
            dispenser->add_source_booster_set_codes( "10E" );

            DraftConfig::Round* round = draftConfig->add_rounds();
            DraftConfig::BoosterRound* boosterRound = round->mutable_booster_round();
            boosterRound->set_selection_time( 60 );
            boosterRound->set_pass_direction( (i%2) == 0 ?
                    DraftConfig::DIRECTION_CLOCKWISE :
                    DraftConfig::DIRECTION_COUNTER_CLOCKWISE );
            DraftConfig::CardDispensation* dispensation = boosterRound->add_dispensations();
            dispensation->set_dispense_all( true );
            dispensation->set_dispenser_index( i );
            for( int i = 0; i < CHAIR_COUNT; ++i )
            {
                dispensation->add_chair_indices( i );
            }
        }

        CATCH_SECTION( "Sunny Day" )
        {
            CATCH_REQUIRE( roomConfigValidator.validate( roomConfig, failureResult ) );
        }

        CATCH_SECTION( "Bad Chair Count" )
        {
            draftConfig->set_chair_count( 0 );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_CHAIR_COUNT );
        }

        CATCH_SECTION( "Bad Bot Count" )
        {
            roomConfig.set_bot_count( 8 );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_BOT_COUNT );
        }

        CATCH_SECTION( "Bad Round Count" )
        {
            draftConfig->clear_rounds();
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_ROUND_COUNT );
        }

        CATCH_SECTION( "Bad Set Code" )
        {
            draftConfig->mutable_dispensers( 0 )->set_source_booster_set_codes( 0, "BADSETCODE" );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_SET_CODE );
        }

        CATCH_SECTION( "Non-booster Set Code" )
        {
            // Invalid to use non-booster code with booster method
            draftConfig->mutable_dispensers( 0 )->set_source_booster_set_codes( 0, "EVG" );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_DISPENSER_CONFIG );
        }

        CATCH_SECTION( "Bad Draft Type - mixed" )
        {
            // Add a sealed round to make draft mixed and invalid (for now).
            DraftConfig::Round* round = draftConfig->add_rounds();
            DraftConfig::SealedRound* sealedRound = round->mutable_sealed_round();
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_DRAFT_TYPE );
        }

        CATCH_SECTION( "Bad Booster Round - no dispensations" )
        {
            DraftConfig::Round* round = draftConfig->mutable_rounds( 0 );
            DraftConfig::BoosterRound* boosterRound = round->mutable_booster_round();
            boosterRound->clear_dispensations();
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_ROUND_CONFIG );
        }

        CATCH_SECTION( "Bad Booster Round - bad dispensation index" )
        {
            DraftConfig::Round* round = draftConfig->mutable_rounds( 0 );
            DraftConfig::BoosterRound* boosterRound = round->mutable_booster_round();
            boosterRound->mutable_dispensations( 0 )->set_dispenser_index( 10 );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_ROUND_CONFIG );
        }
    }

    CATCH_SECTION( "Sealed Rounds" )
    {

        //
        // Create a model RoomConfiguration that test cases can tweak.
        //

        const int CHAIR_COUNT = 8;
        RoomConfig roomConfig;
        roomConfig.set_name( "testroom" );
        roomConfig.set_password_protected( false );
        roomConfig.set_bot_count( 0 );

        DraftConfig* draftConfig = roomConfig.mutable_draft_config();
        draftConfig->set_chair_count( CHAIR_COUNT );

        // Currently this is hardcoded for one sealed round with 6 packs.
        for( int d = 0; d < 6; ++d )
        {
            DraftConfig::CardDispenser* dispenser = draftConfig->add_dispensers();
            dispenser->add_source_booster_set_codes( "10E" );
        }

        DraftConfig::Round* round = draftConfig->add_rounds();
        DraftConfig::SealedRound* sealedRound = round->mutable_sealed_round();
        for( int d = 0; d < 6; ++d )
        {
            DraftConfig::CardDispensation* dispensation = sealedRound->add_dispensations();
            dispensation->set_dispenser_index( d );
            dispensation->set_dispense_all( true );
            for( int i = 0; i < CHAIR_COUNT; ++i )
            {
                dispensation->add_chair_indices( i );
            }
        }

        CATCH_SECTION( "Sunny Day" )
        {
            CATCH_REQUIRE( roomConfigValidator.validate( roomConfig, failureResult ) );
        }

        CATCH_SECTION( "Bad Chair Count" )
        {
            draftConfig->set_chair_count( 0 );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_CHAIR_COUNT );
        }

        CATCH_SECTION( "Bad Bot Count" )
        {
            roomConfig.set_bot_count( 8 );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_BOT_COUNT );
        }

        CATCH_SECTION( "Bad Round Count" )
        {
            draftConfig->clear_rounds();
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_ROUND_COUNT );
        }

        CATCH_SECTION( "Bad Set Code" )
        {
            draftConfig->mutable_dispensers( 0 )->set_source_booster_set_codes( 0, "BADSETCODE" );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_SET_CODE );
        }

        CATCH_SECTION( "Non-booster Set Code" )
        {
            // Invalid to use non-booster code with booster method
            draftConfig->mutable_dispensers( 0 )->set_source_booster_set_codes( 0, "EVG" );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_DISPENSER_CONFIG );
        }

        CATCH_SECTION( "Too many rounds" )
        {
            DraftConfig::Round* round = draftConfig->add_rounds();
            DraftConfig::SealedRound* sealedRound = round->mutable_sealed_round();
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_DRAFT_TYPE );
        }

        CATCH_SECTION( "Bad Sealed Round - no dispensations" )
        {
            DraftConfig::Round* round = draftConfig->mutable_rounds( 0 );
            round->mutable_sealed_round()->clear_dispensations();
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_ROUND_CONFIG );
        }

        CATCH_SECTION( "Bad Sealed Round - bad dispensation index" )
        {
            DraftConfig::Round* round = draftConfig->mutable_rounds( 0 );
            round->mutable_sealed_round()->mutable_dispensations( 0 )->set_dispenser_index( 10 );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_ROUND_CONFIG );
        }
    }

    CATCH_SECTION( "Custom Card Lists" )
    {

        //
        // Create a model RoomConfiguration that test cases can tweak.
        // It contains a single round using a custom card list.
        //

        const int CHAIR_COUNT = 8;
        RoomConfig roomConfig;
        roomConfig.set_name( "testroom" );
        roomConfig.set_password_protected( false );
        roomConfig.set_bot_count( 0 );

        DraftConfig* draftConfig = roomConfig.mutable_draft_config();
        draftConfig->set_chair_count( CHAIR_COUNT );

        DraftConfig::CustomCardList* ccl = draftConfig->add_custom_card_lists();
        ccl->set_name( "test list" );
        DraftConfig::CustomCardList::CardQuantity* q = ccl->add_card_quantities();
        q->set_quantity( 1 );
        q->set_name( "test card" );
        q->set_name( "TST" );

        DraftConfig::CardDispenser* dispenser = draftConfig->add_dispensers();
        dispenser->set_source_custom_card_list_index( 0 );

        // For this test a single round will suffice.
        DraftConfig::Round* round = draftConfig->add_rounds();
        DraftConfig::BoosterRound* boosterRound = round->mutable_booster_round();
        boosterRound->set_selection_time( 60 );
        boosterRound->set_pass_direction( DraftConfig::DIRECTION_CLOCKWISE );
        DraftConfig::CardDispensation* dispensation = boosterRound->add_dispensations();
        dispensation->set_dispenser_index( 0 );
        dispensation->set_quantity( 1 );
        for( int i = 0; i < CHAIR_COUNT; ++i )
        {
            dispensation->add_chair_indices( i );
        }

        CATCH_SECTION( "Sunny Day" )
        {
            CATCH_REQUIRE( roomConfigValidator.validate( roomConfig, failureResult ) );
        }
        CATCH_SECTION( "Bad Index (no list)" )
        {
            draftConfig->clear_custom_card_lists();
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_DISPENSER_CONFIG );
        }
        CATCH_SECTION( "Bad List (no cards)" )
        {
            ccl->clear_card_quantities();
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_CUSTOM_CARD_LIST );
        }
        CATCH_SECTION( "Bad List (no quantity of cards)" )
        {
            q->set_quantity( 0 );
            CATCH_REQUIRE_FALSE( roomConfigValidator.validate( roomConfig, failureResult ) );
            CATCH_REQUIRE( failureResult == CreateRoomFailureRsp::RESULT_INVALID_CUSTOM_CARD_LIST );
        }
    }
}
