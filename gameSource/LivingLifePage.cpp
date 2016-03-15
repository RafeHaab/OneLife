#include "LivingLifePage.h"

#include "objectBank.h"
#include "whiteSprites.h"
#include "message.h"

#include "minorGems/util/SimpleVector.h"


#include "minorGems/game/Font.h"
#include "minorGems/util/stringUtils.h"
#include "minorGems/util/SettingsManager.h"
#include "minorGems/util/random/JenkinsRandomSource.h"
#include "minorGems/game/drawUtils.h"

#include "minorGems/formats/encodingUtils.h"

#include "minorGems/system/Thread.h"


#include <stdlib.h>//#include <math.h>


extern double frameRateFactor;

extern Font *mainFont;
extern Font *handwritingFont;

extern doublePair lastScreenViewCenter;

extern double viewWidth;


static JenkinsRandomSource randSource;

#define CELL_D 128


// base speed for animations that aren't speeded up or slowed down
// when player moving at a different speed, anim speed is modified
#define BASE_SPEED 4.0


int numServerBytesRead = 0;
int numServerBytesSent = 0;

int overheadServerBytesSent = 0;
int overheadServerBytesRead = 0;



SimpleVector<unsigned char> serverSocketBuffer;


// reads all waiting data from socket and stores it in buffer
// returns false on socket error
static char readServerSocketFull( int inServerSocket ) {

    unsigned char buffer[512];
    
    int numRead = readFromSocket( inServerSocket, buffer, 512 );
    
    
    while( numRead > 0 ) {
        serverSocketBuffer.appendArray( buffer, numRead );
        numServerBytesRead += numRead;

        numRead = readFromSocket( inServerSocket, buffer, 512 );
        }    

    if( numRead == -1 ) {
        return false;
        }
    
    return true;
    }



// NULL if there's no full message available
char *getNextServerMessage() {
    // find first terminal character #

    int index = serverSocketBuffer.getElementIndex( '#' );
        
    if( index == -1 ) {
        return NULL;
        }
    
    char *message = new char[ index + 1 ];
    
    for( int i=0; i<index; i++ ) {
        message[i] = (char)( serverSocketBuffer.getElementDirect( 0 ) );
        serverSocketBuffer.deleteElement( 0 );
        }
    // delete message terminal character
    serverSocketBuffer.deleteElement( 0 );
    
    message[ index ] = '\0';
    
    return message;
    }




doublePair gridToDouble( GridPos inGridPos ) {
    doublePair d = { (double) inGridPos.x, (double) inGridPos.y };
    
    return d;
    }



char equal( GridPos inA, GridPos inB ) {
    if( inA.x == inB.x && inA.y == inB.y ) {
        return true;
        }
    return false;
    }


static char isGridAdjacent( int inXA, int inYA, int inXB, int inYB ) {
    if( ( abs( inXA - inXB ) == 1 && inYA == inYB ) 
        ||
        ( abs( inYA - inYB ) == 1 && inXA == inXB ) ) {
        
        return true;
        }

    return false;
    }



static GridPos sub( GridPos inA, GridPos inB ) {
    GridPos result = { inA.x - inB.x, inA.y - inB.y };
    
    return result;
    }




typedef enum messageType {
	MAP_CHUNK,
    MAP_CHANGE,
    PLAYER_UPDATE,
    PLAYER_MOVES_START,
    PLAYER_SAYS,
    FOOD_CHANGE,
    UNKNOWN
    } messageType;




messageType getMessageType( char *inMessage ) {
    char *copy = stringDuplicate( inMessage );
    
    char *firstBreak = strstr( copy, "\n" );
    
    if( firstBreak == NULL ) {
        delete [] copy;
        return UNKNOWN;
        }
    
    firstBreak[0] = '\0';
    
    messageType returnValue = UNKNOWN;

    if( strcmp( copy, "MC" ) == 0 ) {
        returnValue = MAP_CHUNK;
        }
    else if( strcmp( copy, "MX" ) == 0 ) {
        returnValue = MAP_CHANGE;
        }
    else if( strcmp( copy, "PU" ) == 0 ) {
        returnValue = PLAYER_UPDATE;
        }
    else if( strcmp( copy, "PM" ) == 0 ) {
        returnValue = PLAYER_MOVES_START;
        }
    else if( strcmp( copy, "PS" ) == 0 ) {
        returnValue = PLAYER_SAYS;
        }
    else if( strcmp( copy, "FX" ) == 0 ) {
        returnValue = FOOD_CHANGE;
        }
    
    delete [] copy;
    return returnValue;
    }






SimpleVector<LiveObject> gameObjects;




void updateMoveSpeed( LiveObject *inObject ) {
    double etaSec = inObject->moveEtaTime - game_getCurrentTime();
    

    int moveLeft = inObject->pathLength - inObject->currentPathStep - 1;
    

    // count number of turns, which we execute faster than we should
    // because of path smoothing,
    // and use them to reduce overall speed to compensate
    int numTurns = 0;

    if( inObject->currentPathStep < inObject->pathLength - 1 ) {
        GridPos lastDir = sub( 
            inObject->pathToDest[inObject->currentPathStep + 1],
            inObject->pathToDest[inObject->currentPathStep] );
        
        for( int p=inObject->currentPathStep+1; 
             p<inObject->pathLength -1; p++ ) {
            
            GridPos dir = sub( 
                inObject->pathToDest[p+1],
                inObject->pathToDest[p] );

            if( !equal( dir, lastDir ) ) {
                numTurns++;
                lastDir = dir;
                }
            }

        }
    
    double turnTimeBoost = 0.08 * numTurns;

    etaSec += turnTimeBoost;
    
    double speedPerSec = moveLeft / etaSec;
                     
    inObject->currentSpeed = speedPerSec / getRecentFrameRate();
    
    inObject->timeOfLastSpeedUpdate = game_getCurrentTime();
    }


// should match limit on server
static int pathFindingD = 32;


void LivingLifePage::computePathToDest( LiveObject *inObject ) {
    
    if( inObject->pathToDest != NULL ) {
        delete [] inObject->pathToDest;
        inObject->pathToDest = NULL;
        }

    GridPos start;
    start.x = lrint( inObject->currentPos.x );
    start.y = lrint( inObject->currentPos.y );

    GridPos end = { inObject->xd, inObject->yd };
        
    // window around player's start position
    char *blockedMap = new char[ pathFindingD * pathFindingD ];

    int pathOffsetX = pathFindingD/2 - start.x;
    int pathOffsetY = pathFindingD/2 - start.y;


    for( int y=0; y<pathFindingD; y++ ) {
        int mapY = ( y - pathOffsetY ) + mMapD / 2 - mMapOffsetY;
        
        for( int x=0; x<pathFindingD; x++ ) {
            int mapX = ( x - pathOffsetX ) + mMapD / 2 - mMapOffsetX;
            
            // note that unknowns (-1) count as blocked too
            blockedMap[ y * pathFindingD + x ]
                =
                ( mMap[ mapY * mMapD + mapX ] != 0 );
            }
        }
    
    
    
    start.x += pathOffsetX;
    start.y += pathOffsetY;
    
    end.x += pathOffsetX;
    end.y += pathOffsetY;
    
    double startTime = game_getCurrentTime();

    GridPos closestFound;
    
    char pathFound = 
        pathFind( pathFindingD, pathFindingD,
                  blockedMap, 
                  start, end, 
                  &( inObject->pathLength ),
                  &( inObject->pathToDest ),
                  &closestFound );
    
    if( pathFound && inObject->pathToDest != NULL ) {
        printf( "Path found in %f ms\n", 
                1000 * ( game_getCurrentTime() - startTime ) );

        // move into world coordinates
        for( int i=0; i<inObject->pathLength; i++ ) {
            inObject->pathToDest[i].x -= pathOffsetX;
            inObject->pathToDest[i].y -= pathOffsetY;
            }

        GridPos aGridPos = inObject->pathToDest[0];
        GridPos bGridPos = inObject->pathToDest[1];
        
        doublePair aPos = { (double)aGridPos.x, (double)aGridPos.y };
        doublePair bPos = { (double)bGridPos.x, (double)bGridPos.y };
        
        inObject->currentMoveDirection =
            normalize( sub( bPos, aPos ) );
        }
    else {
        printf( "Path not found in %f ms\n", 
                1000 * ( game_getCurrentTime() - startTime ) );
        
        if( !pathFound ) {
            
            inObject->closestDestIfPathFailedX = 
                closestFound.x - pathOffsetX;
            
            inObject->closestDestIfPathFailedY = 
                closestFound.y - pathOffsetY;
            }
        else {
            // degen case where start == end?
            inObject->closestDestIfPathFailedX = inObject->xd;
            inObject->closestDestIfPathFailedY = inObject->yd;
            }
        
        
        }
    
    inObject->currentPathStep = 0;
    
    delete [] blockedMap;
    }




static void addNewAnim( LiveObject *inObject, AnimType inNewAnim ) {
    if( inObject->curAnim != inNewAnim ) {
        if( inObject->lastAnimFade != 0 ) {
                        
            // don't double stack
            if( inObject->futureAnimStack.size() == 0 ||
                inObject->futureAnimStack.getElementDirect(
                    inObject->futureAnimStack.size() - 1 ) 
                != inNewAnim ) {
                
                inObject->futureAnimStack.push_back( inNewAnim );
                }
            }
        else {
            inObject->lastAnim = inObject->curAnim;
            inObject->curAnim = inNewAnim;
            inObject->lastAnimFade = 1;
            inObject->animationFrozenRotFrameCount = 
                inObject->animationFrameCount;
            }
        }


    if( inObject->curHeldAnim != inNewAnim ) {

        // special case:
        // looks better if we just freeze moving anim when we stop, if we can
        
        if( inObject->holdingID != 0 &&
            inObject->curHeldAnim == moving &&
            inNewAnim == held &&
            isAnimEmpty( inObject->holdingID, held ) ) {
            
            inObject->heldAnimationFrameFrozen = true;
        
            // just keep moving animation in place
            }
        else {
            inObject->heldAnimationFrameFrozen = false;
        

            if( inObject->lastHeldAnimFade != 0 ) {
                        
                // don't double stack
                if( inObject->futureHeldAnimStack.size() == 0 ||
                    inObject->futureHeldAnimStack.getElementDirect(
                        inObject->futureHeldAnimStack.size() - 1 ) 
                    != inNewAnim ) {
                    
                    inObject->futureHeldAnimStack.push_back( inNewAnim );
                    }
                }
            else {
                inObject->lastHeldAnim = inObject->curHeldAnim;
                inObject->curHeldAnim = inNewAnim;
                inObject->lastHeldAnimFade = 1;
                inObject->heldAnimationFrozenRotFrameCount = 
                inObject->heldAnimationFrameCount;
                }
            }
        }
    else if( inNewAnim == moving ) {
        // moving never frozen
        // we may need to unfreeze if we kept our moving frozen last
        // time we transitioned to held
        
        inObject->heldAnimationFrameFrozen = false;
        }
    
        
    }






// if user clicks to initiate an action while still moving, we
// queue it here
static char *nextActionMessageToSend = NULL;

// block move until next PLAYER_UPDATE received after action sent
static char playerActionPending = false;
static int playerActionTargetX, playerActionTargetY;
static char playerActionTargetNotAdjacent = false;


int ourID;

char lastCharUsed = 'A';




LivingLifePage::LivingLifePage() 
        : mServerAddress( NULL ),
          mServerSocket( -1 ), 
          mFirstServerMessagesReceived( 0 ),
          mMapD( 64 ),
          mMapOffsetX( 0 ),
          mMapOffsetY( 0 ),
          mEKeyDown( false ),
          mFoodEmptySprite( loadSprite( "hungerEmpty.tga", false ) ),
          mFoodFullSprite( loadSprite( "hungerFull.tga", false ) ),
          mLastMouseOverID( 0 ),
          mCurMouseOverID( 0 ),
          mLastMouseOverFade( 0.0 ),
          mChalkBlotSprite( loadWhiteSprite( "chalkBlot.tga" ) ),
          mSayField( handwritingFont, 0, 1000, 10, true, NULL,
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ.,'?! " ) {
    
    // not visible, drawn under world at 0, 0, and doesn't move with camera
    // still, we can use it to receive/process/filter typing events
    addComponent( &mSayField );
    
    mSayField.unfocus();
    

    
    mServerAddress = SettingsManager::getStringSetting( "serverAddress" );

    if( mServerAddress == NULL ) {
        mServerAddress = stringDuplicate( "127.0.0.1" );
        }

    mServerPort = SettingsManager::getIntSetting( "serverPort", 5077 );



    mMap = new int[ mMapD * mMapD ];
    
    mMapContainedStacks = new SimpleVector<int>[ mMapD * mMapD ];
    
    mMapAnimationFrameCount =  new int[ mMapD * mMapD ];
    mMapCurAnimType =  new AnimType[ mMapD * mMapD ];
    mMapLastAnimType =  new AnimType[ mMapD * mMapD ];
    mMapLastAnimFade =  new double[ mMapD * mMapD ];
    
    mMapDropOffsets = new doublePair[ mMapD * mMapD ];
    mMapTileFlips = new char[ mMapD * mMapD ];

    for( int i=0; i<mMapD *mMapD; i++ ) {
        // -1 represents unknown
        // 0 represents known empty
        mMap[i] = -1;
        mMapAnimationFrameCount[i] = randSource.getRandomBoundedInt( 0, 500 );
        mMapCurAnimType[i] = ground;
        mMapLastAnimType[i] = ground;
        mMapLastAnimFade[i] = 0;

        mMapDropOffsets[i].x = 0;
        mMapDropOffsets[i].y = 0;
        
        mMapTileFlips[i] = false;
        }

    }




void LivingLifePage::clearLiveObjects() {
    for( int i=0; i<gameObjects.size(); i++ ) {
        
        LiveObject *nextObject =
            gameObjects.getElement( i );
        
        if( nextObject->containedIDs != NULL ) {
            delete [] nextObject->containedIDs;
            }
        
        if( nextObject->pathToDest != NULL ) {
            delete [] nextObject->pathToDest;
            }

        if( nextObject->currentSpeech != NULL ) {
            delete [] nextObject->currentSpeech;
            }
        }
    
    gameObjects.deleteAll();
    }




LivingLifePage::~LivingLifePage() {
    printf( "Total received = %d bytes (+%d in headers), "
            "total sent = %d bytes (+%d in headers)\n",
            numServerBytesRead, overheadServerBytesRead,
            numServerBytesSent, overheadServerBytesSent );

    
    clearLiveObjects();


    if( mServerAddress != NULL ) {    
        delete [] mServerAddress;
        mServerAddress = NULL;
        }
    
    if( mServerSocket != -1 ) {
        closeSocket( mServerSocket );
        }

    
    delete [] mMapAnimationFrameCount;
    delete [] mMapCurAnimType;
    delete [] mMapLastAnimType;
    delete [] mMapLastAnimFade;
    
    delete [] mMapDropOffsets;

    delete [] mMapTileFlips;
    
    delete [] mMapContainedStacks;
    
    delete [] mMap;

    delete [] nextActionMessageToSend;


    freeSprite( mFoodEmptySprite );
    freeSprite( mFoodFullSprite );
    freeSprite( mChalkBlotSprite );
    }



LiveObject *LivingLifePage::getOurLiveObject() {
    
    LiveObject *ourLiveObject = NULL;

    for( int i=0; i<gameObjects.size(); i++ ) {
        
        LiveObject *o = gameObjects.getElement( i );
        
        if( o->id == ourID ) {
            ourLiveObject = o;
            break;
            }
        }
    return ourLiveObject;
    }




// forces uppercase
void LivingLifePage::drawChalkBackgroundString( doublePair inPos, 
                                                const char *inString,
                                                double inFade,
                                                double inMaxWidth,
                                                int inForceMinChalkBlots ) {
    
    char *stringUpper = stringToUpperCase( inString );

    // break into lines
    SimpleVector<char *> *tokens = 
        tokenizeString( stringUpper );
    
    delete [] stringUpper;
    
    // collect all lines before drawing them
    SimpleVector<char *> lines;
    
    
    if( tokens->size() > 0 ) {
        // start with firt token
        char *firstToken = tokens->getElementDirect( 0 );
        
        lines.push_back( firstToken );
        
        tokens->deleteElement( 0 );
        }
    
    
    while( tokens->size() > 0 ) {
        char *nextToken = tokens->getElementDirect( 0 );
        
        char *currentLine = lines.getElementDirect( lines.size() - 1 );
         
        char *expandedLine = autoSprintf( "%s %s", currentLine, nextToken );
         
        if( handwritingFont->measureString( expandedLine ) <= inMaxWidth ) {
            // replace current line
            delete [] currentLine;
            lines.deleteElement(  lines.size() - 1 );
             
            lines.push_back( expandedLine );
            }
        else {
            // expanded is too long
            // put token at start of next line
            delete [] expandedLine;
             
            lines.push_back( stringDuplicate( nextToken ) );
            }
         

        delete [] nextToken;
        tokens->deleteElement( 0 );
        }
    
    delete tokens;
    
    if( lines.size() == 0 ) {
        return;
        }

    double lineSpacing = handwritingFont->getFontHeight() + 5;
    
    double firstLineY =  inPos.y + ( lines.size() - 1 ) * lineSpacing;
    

    setDrawColor( 1, 1, 1, inFade );

    // with a fixed seed
    JenkinsRandomSource blotRandSource( 0 );
        
    for( int i=0; i<lines.size(); i++ ) {
        char *line = lines.getElementDirect( i );
        

        double length = handwritingFont->measureString( line );
            
        int numBlots = lrint( 0.25 + length / 20 ) + 1;
        
        if( inForceMinChalkBlots != -1 && numBlots < inForceMinChalkBlots ) {
            numBlots = inForceMinChalkBlots;
            }
    
        doublePair blotSpacing = { 20, 0 };
    
        doublePair firstBlot = 
            { inPos.x, firstLineY - i * lineSpacing};

        
        for( int b=0; b<numBlots; b++ ) {
            doublePair blotPos = add( firstBlot, mult( blotSpacing, b ) );
        
            double rot = blotRandSource.getRandomDouble();
            drawSprite( mChalkBlotSprite, blotPos, 1.0, rot );
            drawSprite( mChalkBlotSprite, blotPos, 1.0, rot );
            }
        }
    
    
    setDrawColor( 0, 0, 0, inFade );

    for( int i=0; i<lines.size(); i++ ) {
        char *line = lines.getElementDirect( i );
        
        doublePair lineStart = 
            { inPos.x, firstLineY - i * lineSpacing};
        
        handwritingFont->drawString( line, lineStart, alignLeft );
        delete [] line;
        }
    
    }


SimpleVector<doublePair> trail;



void LivingLifePage::draw( doublePair inViewCenter, 
                           double inViewSize ) {
    
    if( gameObjects.size() == 0 ) {
        // haven't gotten first message from server yet
        
        // draw this to cover up utility text field, but not
        // waiting icon at top
        setDrawColor( 0, 0, 0, 1 );
        drawSquare( lastScreenViewCenter, 100 );
        
        setDrawColor( 1, 1, 1, 1 );
        doublePair pos = { 0, 0 };
        drawMessage( "waitingBirth", pos );
        return;
        }
    

    setDrawColor( 1, 1, 1, 1 );
    drawSquare( lastScreenViewCenter, viewWidth );
    
    //if( currentGamePage != NULL ) {
    //    currentGamePage->base_draw( lastScreenViewCenter, viewWidth );
    //    }
    
    setDrawColor( 1, 1, 1, 1 );

    int gridCenterX = 
        lrintf( lastScreenViewCenter.x / CELL_D ) - mMapOffsetX + mMapD/2;
    int gridCenterY = 
        lrintf( lastScreenViewCenter.y / CELL_D ) - mMapOffsetY + mMapD/2;
    
    int xStart = gridCenterX - 8;
    int xEnd = gridCenterX + 8;

    int yStart = gridCenterY - 5;
    int yEnd = gridCenterY + 5;

    if( xStart < 0 ) {
        xStart = 0;
        }
    if( xStart >= mMapD ) {
        xStart = mMapD - 1;
        }
    
    if( yStart < 0 ) {
        yStart = 0;
        }
    if( yStart >= mMapD ) {
        yStart = mMapD - 1;
        }

    if( xEnd < 0 ) {
        xEnd = 0;
        }
    if( xEnd >= mMapD ) {
        xEnd = mMapD - 1;
        }
    
    if( yStart < 0 ) {
        yStart = 0;
        }
    if( yEnd >= mMapD ) {
        yEnd = mMapD - 1;
        }
    

    
    int worldXStart = xStart + mMapOffsetX - mMapD / 2;
    int worldXEnd = xEnd + mMapOffsetX - mMapD / 2;

    //int worldYStart = xStart + mMapOffsetY - mMapD / 2;
    //int worldYEnd = xEnd + mMapOffsetY - mMapD / 2;


    // capture pointers to objects that are speaking and visible on 
    // screen

    // draw speech on top of everything at end

    SimpleVector<LiveObject *> speakers;
    SimpleVector<doublePair> speakersPos;


    // FIXME:  skip these that are off screen
    // of course, we may not end up drawing paths on screen anyway
    // (probably only our own destination), so don't fix this for now

    // draw paths and destinations under everything
    for( int i=0; i<gameObjects.size(); i++ ) {
        
        LiveObject *o = gameObjects.getElement( i );


        if( o->currentPos.x != o->xd || o->currentPos.y != o->yd ) {
            // destination
            
            char *string = autoSprintf( "[%c]", o->displayChar );
        
            doublePair pos;
            pos.x = o->xd * CELL_D;
            pos.y = o->yd * CELL_D;
        
            setDrawColor( 1, 0, 0, 1 );
            mainFont->drawString( string, 
                                  pos, alignCenter );
            delete [] string;


            if( o->pathToDest != NULL ) {
                // highlight path
                
                for( int p=0; p< o->pathLength; p++ ) {
                    GridPos pathSpot = o->pathToDest[ p ];
                    
                    pos.x = pathSpot.x * CELL_D;
                    pos.y = pathSpot.y * CELL_D;

                    setDrawColor( 1, 1, 0, 1 );
                    mainFont->drawString( "P", 
                                          pos, alignCenter );
                    }
                }
            else {
                pos.x = o->closestDestIfPathFailedX * CELL_D;
                pos.y = o->closestDestIfPathFailedY * CELL_D;
                
                setDrawColor( 1, 0, 1, 1 );
                mainFont->drawString( "P", 
                                      pos, alignCenter );
                }
            
            
            }
        }
    


    for( int y=yEnd; y>=yStart; y-- ) {
        
        int screenY = CELL_D * ( y + mMapOffsetY - mMapD / 2 );




        // draw any player objects that are in this row
        // UNDER the objects in this row (so that pick up and set down
        // looks correct

        int worldY = y + mMapOffsetY - mMapD / 2;
        

        for( int i=0; i<gameObjects.size(); i++ ) {
        
            LiveObject *o = gameObjects.getElement( i );
            
            // first, check if it's on the screen
            int oX = o->xd;
            int oY = o->yd;
            
            if( o->currentSpeed != 0 ) {
                oX = o->pathToDest[ o->currentPathStep ].x;
                oY = o->pathToDest[ o->currentPathStep ].y;
                }
            
            if( oY == worldY && oX >= worldXStart && oX <= worldXEnd ) {
                // in this row
                


                // current pos
                char *string = autoSprintf( "%c", o->displayChar );
                
                doublePair pos = mult( o->currentPos, CELL_D );
                
                doublePair actionOffset = { 0, 0 };
                
                //trail.push_back( pos );
                
                if( o->id == ourID && 
                    o->pendingActionAnimationProgress != 0 ) {
                    
                    // wiggle toward target
                    
                    float xDir = 0;
                    float yDir = 0;
                    
                    if( o->xd < playerActionTargetX ) {
                        xDir = 1;
                        o->holdingFlip = false;
                        }
                    if( o->xd > playerActionTargetX ) {
                        xDir = -1;
                        o->holdingFlip = true;
                        }
                    if( o->yd < playerActionTargetY ) {
                        yDir = 1;
                        }
                    if( o->yd > playerActionTargetY ) {
                        yDir = -1;
                        }
                    
                    double offset =
                        32 - 
                        32 * 
                        cos( 2 * M_PI * o->pendingActionAnimationProgress );
                    
                    
                    actionOffset.x += xDir * offset;
                    actionOffset.y += yDir * offset;
                    }
                
                
                // bare hands action OR holding something
                // character wiggle
                if(  o->id == ourID && 
                     o->pendingActionAnimationProgress != 0 ) {
                    
                    pos = add( pos, actionOffset );
                    }
                
                
                float red, blue;
                
                red = 0;
                blue = 0;
                
                if( o->heat > 0.75 ) {
                    red = 1;
                    }
                else if( o->heat < 0.25 ) {
                    blue = 1;
                    }
                

                
                AnimType curType = o->curAnim;
                AnimType fadeTargetType = o->curAnim;
                
                double animFade = 1.0;
                
                double timeVal = frameRateFactor * 
                    o->animationFrameCount / 60.0;
                
                double rotTimeVal = timeVal;

                if( o->lastAnimFade > 0 ) {
                    curType = o->lastAnim;
                    fadeTargetType = o->curAnim;
                    animFade = o->lastAnimFade;
                    
                    rotTimeVal = frameRateFactor * 
                        o->animationFrozenRotFrameCount / 60.0;
                    }
                

                setDrawColor( 1, 1, 1, 1 );
                //setDrawColor( red, 0, blue, 1 );
                //mainFont->drawString( string, 
                //                      pos, alignCenter );
                
                double age = o->age + 
                    o->ageRate * ( game_getCurrentTime() - o->lastAgeSetTime );

                char holdingSomething = 
                    ( o->holdingID != 0 );
                
                drawObjectAnim( o->displayID, curType, 
                                timeVal, rotTimeVal,
                                animFade,
                                fadeTargetType,
                                pos,
                                o->holdingFlip,
                                age,
                                holdingSomething,
                                o->clothing );

                delete [] string;
                
                if( o->holdingID != 0 ) { 
                    doublePair holdPos = pos;

                    ObjectRecord *heldObject = getObject( o->holdingID );
                    
                    if( o->holdingFlip ) {
                        holdPos.x -= heldObject->heldOffset.x;
                        }
                    else {
                        holdPos.x += heldObject->heldOffset.x;
                        }
                    
                    holdPos.y += heldObject->heldOffset.y;
                    
                    holdPos = mult( holdPos, 1.0 / CELL_D );

                    if( o->heldPosOverride && 
                        ! equal( holdPos, o->heldObjectPos ) ) {
                        
                        doublePair delta = sub( holdPos, o->heldObjectPos );
                        
                        double step = frameRateFactor * 0.0625;
                        
                        if( length( delta ) < step ) {
                            o->heldObjectPos = holdPos;
                            }
                        else {
                            o->heldObjectPos =
                                add( o->heldObjectPos,
                                     mult( normalize( delta ),
                                           step ) );
                            
                            holdPos = o->heldObjectPos;
                            }
                        }
                    else {
                        o->heldPosOverride = false;
                        // track it every frame so we have a good
                        // base point for smooth move when the object
                        // is dropped
                        o->heldObjectPos = holdPos;
                        }
                    
                    holdPos = mult( holdPos, CELL_D );

                    setDrawColor( 1, 1, 1, 1 );
                    
                    
                    AnimType curHeldType = o->curHeldAnim;
                    AnimType fadeTargetHeldType = o->curHeldAnim;
                
                    double heldAnimFade = 1.0;
                    
                    double heldTimeVal = frameRateFactor * 
                        o->heldAnimationFrameCount / 60.0;

                    double heldRotTimeVal = heldTimeVal;

                    if( o->lastHeldAnimFade > 0 ) {
                        curHeldType = o->lastHeldAnim;
                        fadeTargetHeldType = o->curHeldAnim;
                        heldAnimFade = o->lastHeldAnimFade;

                        heldRotTimeVal = frameRateFactor * 
                            o->heldAnimationFrozenRotFrameCount / 60.0;
                        }

                        
                    

                    if( o->numContained == 0 ) {
                        
                        drawObjectAnim( o->holdingID, curHeldType, 
                                        heldTimeVal, heldRotTimeVal,
                                        heldAnimFade,
                                        fadeTargetHeldType,
                                        holdPos,
                                        o->holdingFlip, -1, false,
                                        getEmptyClothingSet() );
                        }
                    else {
                        drawObjectAnim( o->holdingID, curHeldType, 
                                        heldTimeVal, heldTimeVal,
                                        heldAnimFade,
                                        fadeTargetHeldType,
                                        holdPos,
                                        o->holdingFlip,
                                        -1, false,
                                        getEmptyClothingSet(),
                                        o->numContained,
                                        o->containedIDs );
                        }
                    }
                
                if( o->currentSpeech != NULL ) {
                    
                    speakers.push_back( o );
                    speakersPos.push_back( pos );
                    }
                }
            }



        // now draw tiles in this row
        for( int x=xStart; x<=xEnd; x++ ) {
            
            int screenX = CELL_D * ( x + mMapOffsetX - mMapD / 2 );

            int mapI = y * mMapD + x;
            
            int oID = mMap[ mapI ];
            
            if( oID > 0 ) {
                
                mMapAnimationFrameCount[ mapI ] ++;
                
                if( mMapLastAnimFade[ mapI ] > 0 ) {
                    mMapLastAnimFade[ mapI ] -= 0.05 * frameRateFactor;
                    if( mMapLastAnimFade[ mapI ] < 0 ) {
                        mMapLastAnimFade[ mapI ] = 0;
                        // start current animation fresh after fade
                        mMapAnimationFrameCount[ mapI ] = 0;

                        if( mMapCurAnimType[ mapI ] != ground ) {
                            // transition to ground now
                            mMapLastAnimType[ mapI ] = mMapCurAnimType[ mapI ];
                            mMapCurAnimType[ mapI ] = ground;
                            mMapLastAnimFade[ mapI ] = 1;
                            }
                        }
                    }
                

                doublePair pos = { (double)screenX, (double)screenY };

                if( mMapDropOffsets[ mapI ].x != 0 ||
                    mMapDropOffsets[ mapI ].y != 0 ) {
                    
                    pos = add( pos, mult( mMapDropOffsets[ mapI ], CELL_D ) );
                    
                    doublePair nullOffset = { 0, 0 };
                    

                    doublePair delta = sub( nullOffset, 
                                            mMapDropOffsets[ mapI ] );
                    
                    double step = frameRateFactor * 0.0625;
                    
                    if( length( delta ) < step ) {
                        
                        mMapDropOffsets[ mapI ].x = 0;
                        mMapDropOffsets[ mapI ].y = 0;
                        }
                    else {
                        mMapDropOffsets[ mapI ] =
                            add( mMapDropOffsets[ mapI ],
                                 mult( normalize( delta ), step ) );
                        }
                    }
                

                setDrawColor( 1, 1, 1, 1 );
                
                AnimType curType = ground;
                AnimType fadeTargetType = ground;
                double animFade = 1;
                
                if( mMapLastAnimFade[ mapI ] != 0 ) {
                    animFade = mMapLastAnimFade[ mapI ];
                    curType = mMapLastAnimType[ mapI ];
                    fadeTargetType = mMapCurAnimType[ mapI ];
                    }
                
                double timeVal = frameRateFactor * 
                    mMapAnimationFrameCount[ mapI ] / 60.0;
                

                if( mMapContainedStacks[ mapI ].size() > 0 ) {
                    int *stackArray = 
                        mMapContainedStacks[ mapI ].getElementArray();
                    
                    drawObjectAnim( oID, 
                                    curType, timeVal, timeVal,
                                    animFade,
                                    fadeTargetType,
                                    pos, mMapTileFlips[ mapI ],
                                    -1,
                                    false,
                                    getEmptyClothingSet(),
                                    mMapContainedStacks[ mapI ].size(),
                                    stackArray );
                    delete [] stackArray;
                    }
                else {
                    drawObjectAnim( oID, 
                                    curType, timeVal, timeVal,
                                    animFade,
                                    fadeTargetType, pos, 
                                    mMapTileFlips[ mapI ], -1,
                                    false,
                                    getEmptyClothingSet() );
                    }
                }
            else if( oID == -1 ) {
                // unknown
                doublePair pos = { (double)screenX, (double)screenY };
                
                setDrawColor( 0, 0, 0, 0.5 );
                drawSquare( pos, 14 );
                }
            }

        } // end loop over rows on screen

    
    for( int i=0; i<speakers.size(); i++ ) {
        LiveObject *o = speakers.getElementDirect( i );
        
        doublePair pos = speakersPos.getElementDirect( i );
        

        doublePair speechPos = pos;
                    
        int width = 250;
        int widthLimit = 250;
        
        double fullWidth = 
            handwritingFont->measureString( o->currentSpeech );
        
        if( fullWidth < width ) {
            width = (int)fullWidth;
            }
        
        
        speechPos.x -= width / 2;
        speechPos.y += 130;
        
        drawChalkBackgroundString( speechPos, o->currentSpeech, 
                                   o->speechFade, widthLimit );
        }
    

        
    doublePair lastChunkCenter = { (double)( CELL_D * mMapOffsetX ), 
                                   (double)( CELL_D * mMapOffsetY ) };
    
    setDrawColor( 0, 1, 0, 1 );
    
    mainFont->drawString( "X", 
                          lastChunkCenter, alignCenter );
    
    




    
    setDrawColor( 0, 0.5, 0, 0.25 );
    if( false )for( int i=0; i<trail.size(); i++ ) {
        doublePair *p = trail.getElement( i );
        
        mainFont->drawString( ".", 
                              *p, alignCenter );
        }
    

    setDrawColor( 0, 0, 0, 0.125 );
    
    int screenGridOffsetX = lrint( lastScreenViewCenter.x / CELL_D );
    int screenGridOffsetY = lrint( lastScreenViewCenter.y / CELL_D );
    
    for( int y=-5; y<=5; y++ ) {
        for( int x=-8; x<=8; x++ ) {
            
            doublePair pos;
            pos.x = ( x + screenGridOffsetX ) * CELL_D;
            pos.y = ( y + screenGridOffsetY ) * CELL_D;
            
            drawSquare( pos, CELL_D * .45 );
            }
        }    


    // info panel at bottom
    setDrawColor( 0.5, 0.5, 0.5, .75 );
    drawRect( lastScreenViewCenter.x - 333, lastScreenViewCenter.y - 300, 
              lastScreenViewCenter.x + 333, lastScreenViewCenter.y - 333 );
    
    LiveObject *ourLiveObject = getOurLiveObject();

    if( ourLiveObject != NULL ) {
        
        if( ourLiveObject->foodStore <= ourLiveObject->foodCapacity / 2 ) {
            setDrawColor( 1, .5, .5, 1 );
            }
        else {
            setDrawColor( 1, 1, 1, 1 );
            }
    

        for( int i=0; i<ourLiveObject->foodCapacity; i++ ) {
            doublePair pos = { lastScreenViewCenter.x - 300, 
                               lastScreenViewCenter.y - 317 };
        
            pos.x += i * 32;
            
            if( i < ourLiveObject->foodStore ) {                
                drawSprite( mFoodFullSprite, pos );
                }
            else {
                drawSprite( mFoodEmptySprite, pos );
                }
            }
        
        setDrawColor( 0, 0, 0, 1 );
        
        doublePair pos = { lastScreenViewCenter.x - 300, 
                           lastScreenViewCenter.y - 317 };
        pos.x += 128;
        

        char *heatString = autoSprintf( "%.2f", ourLiveObject->heat );
        mainFont->drawString( heatString, pos, alignCenter );
        delete [] heatString;

        if( mCurMouseOverID != 0 || mLastMouseOverID != 0 ) {
            int idToDescribe = mCurMouseOverID;
            
            double fade = 1.0;
            
            if( mCurMouseOverID == 0 ) {
                idToDescribe = mLastMouseOverID;
                fade = mLastMouseOverFade;
                }

            
            
            doublePair pos = { lastScreenViewCenter.x - 300, 
                               lastScreenViewCenter.y - 317 };
            char *des = getObject( idToDescribe )->description;

            drawChalkBackgroundString( pos, des, fade, 10 );
            }
        

        if( mSayField.isFocused() ) {
            char *partialSay = mSayField.getText();
            
            int charsLeft = mSayField.getMaxLength() - strlen( partialSay );

            char *drawString = autoSprintf( "SAY:  %s\n", 
                                            partialSay, charsLeft );
            delete [] partialSay;

            doublePair pos = { lastScreenViewCenter.x, 
                               lastScreenViewCenter.y - 317 };
            
            drawChalkBackgroundString( pos, drawString, 1.0, 250 );
            
            delete [] drawString;

            drawString = autoSprintf( "%d\n", charsLeft );
            
            pos.x -= 64;
            
            drawChalkBackgroundString( pos, drawString, 1.0, 250, 2 );

            delete [] drawString;
            }
        
        }
    
    }




        
void LivingLifePage::step() {
    
    if( mServerSocket == -1 ) {
        mServerSocket = openSocketConnection( mServerAddress, mServerPort );
        return;
        }
    

    double pageLifeTime = game_getCurrentTime() - mPageStartTime;
    
    if( pageLifeTime < 1 ) {
        return;
        }
    else {
        setWaiting( false );
        }


    // first, read all available data from server
    char readSuccess = readServerSocketFull( mServerSocket );
    

    if( ! readSuccess ) {
        closeSocket( mServerSocket );
        mServerSocket = -1;
        setSignal( "died" );
        return;
        }
    

    char *message = getNextServerMessage();


    while( message != NULL ) {
        overheadServerBytesRead += 52;
        
        printf( "Got length %d message\n%s\n", strlen( message ), message );

        messageType type = getMessageType( message );
        

        if( type == MAP_CHUNK ) {
            
            int size = 0;
            int x = 0;
            int y = 0;
            
            int binarySize = 0;
            int compressedSize = 0;
            
            sscanf( message, "MC\n%d %d %d\n%d %d\n", 
                    &size, &x, &y, &binarySize, &compressedSize );
            
            printf( "Got map chunk with bin size %d, compressed size %d\n", 
                    binarySize, compressedSize );
            
            // recenter our in-ram sub-map around this new chunk
            int newMapOffsetX = x + size/2;
            int newMapOffsetY = y + size/2;
            
            // move old cached map cells over to line up with new center

            int xMove = mMapOffsetX - newMapOffsetX;
            int yMove = mMapOffsetY - newMapOffsetY;
            
            int *newMap = new int[ mMapD * mMapD ];
            

            int *newMapAnimationFrameCount = new int[ mMapD * mMapD ];
        
            AnimType *newMapCurAnimType = new AnimType[ mMapD * mMapD ];
            AnimType *newMapLastAnimType = new AnimType[ mMapD * mMapD ];
            double *newMapLastAnimFade = new double[ mMapD * mMapD ];
            
            doublePair *newMapDropOffsets = new doublePair[ mMapD * mMapD ];
            char *newMapTileFlips= new char[ mMapD * mMapD ];

            
            for( int i=0; i<mMapD *mMapD; i++ ) {
                // starts uknown, not empty
                newMap[i] = -1;
                
                newMapAnimationFrameCount[i] = 
                    randSource.getRandomBoundedInt( 0, 500 );;
                newMapCurAnimType[i] = ground;
                newMapLastAnimFade[i] = ground;
                newMapLastAnimFade[i] = 0;
                newMapDropOffsets[i].x = 0;
                newMapDropOffsets[i].y = 0;

                newMapTileFlips[i] = false;
                

                int newX = i % mMapD;
                int newY = i / mMapD;
                
                int oldX = newX - xMove;
                int oldY = newY - yMove;
                
                if( oldX >= 0 && oldX < mMapD
                    &&
                    oldY >= 0 && oldY < mMapD ) {
                    
                    int oI = oldY * mMapD + oldX;

                    newMap[i] = mMap[oI];

                    newMapAnimationFrameCount[i] = mMapAnimationFrameCount[oI];
                    newMapCurAnimType[i] = mMapCurAnimType[oI];
                    newMapLastAnimFade[i] = mMapLastAnimFade[oI];
                    newMapLastAnimFade[i] = mMapLastAnimFade[oI];
                    newMapDropOffsets[i] = mMapDropOffsets[oI];

                    newMapTileFlips[i] = mMapTileFlips[oI];
                    }
                }
            
            memcpy( mMap, newMap, mMapD * mMapD * sizeof( int ) );

            memcpy( mMapAnimationFrameCount, newMapAnimationFrameCount, 
                    mMapD * mMapD * sizeof( int ) );
            memcpy( mMapCurAnimType, newMapCurAnimType, 
                    mMapD * mMapD * sizeof( AnimType ) );
            memcpy( mMapLastAnimFade, newMapLastAnimFade,
                    mMapD * mMapD * sizeof( AnimType ) );
            memcpy( mMapLastAnimFade, newMapLastAnimFade,
                    mMapD * mMapD * sizeof( double ) );
            memcpy( mMapDropOffsets, newMapDropOffsets,
                    mMapD * mMapD * sizeof( doublePair ) );
            
            memcpy( mMapTileFlips, newMapTileFlips,
                    mMapD * mMapD * sizeof( char ) );
            
            delete [] newMap;
            delete [] newMapAnimationFrameCount;
            delete [] newMapCurAnimType;
            delete [] newMapLastAnimType;
            delete [] newMapLastAnimFade;
            delete [] newMapDropOffsets;
            delete [] newMapTileFlips;
            
            

            mMapOffsetX = newMapOffsetX;
            mMapOffsetY = newMapOffsetY;
            
            
            unsigned char *compressedChunk = 
                new unsigned char[ compressedSize ];
    
            
            for( int i=0; i<compressedSize; i++ ) {
                compressedChunk[i] = serverSocketBuffer.getElementDirect( 0 );
                
                serverSocketBuffer.deleteElement( 0 );
                }

            
            unsigned char *decompressedChunk =
                zipDecompress( compressedChunk, 
                               compressedSize,
                               binarySize );
            
            delete [] compressedChunk;
            
            
            unsigned char *binaryChunk = new unsigned char[ binarySize + 1 ];
            
            memcpy( binaryChunk, decompressedChunk, binarySize );
            
            delete [] decompressedChunk;
 
            
            // for now, binary chunk is actually just ASCII
            binaryChunk[ binarySize ] = '\0';
            

            SimpleVector<char *> *tokens = 
                tokenizeString( (char*)binaryChunk );
            
            delete [] binaryChunk;


            int numCells = size * size;

            if( tokens->size() == numCells ) {
                
                for( int i=0; i<tokens->size(); i++ ) {
                    int cX = i % size;
                    int cY = i / size;
                    
                    int mapX = cX + x - mMapOffsetX + mMapD / 2;
                    int mapY = cY + y - mMapOffsetY + mMapD / 2;
                    
                    if( mapX >= 0 && mapX < mMapD
                        &&
                        mapY >= 0 && mapY < mMapD ) {
                        
                        
                        int mapI = mapY * mMapD + mapX;
                        
                        sscanf( tokens->getElementDirect(i),
                                "%d", &( mMap[mapI] ) );

                        mMapContainedStacks[mapI].deleteAll();
                        

                        if( strstr( tokens->getElementDirect(i), "," ) 
                            != NULL ) {
                            
                            int numInts;
                            char **ints = split( tokens->getElementDirect(i), 
                                                 ",", &numInts );
                            
                            delete [] ints[0];

                            int numContained = numInts - 1;

                            for( int c=0; c<numContained; c++ ) {
                                int contained = atoi( ints[ c + 1 ] );
                                mMapContainedStacks[mapI].push_back( 
                                    contained );
                                
                                delete [] ints[ c + 1 ];
                                }
                            delete [] ints;
                            }
                        }
                    }
                }   
            
            tokens->deallocateStringElements();
            delete tokens;

            mFirstServerMessagesReceived |= 1;
            }
        else if( type == MAP_CHANGE ) {
            int numLines;
            char **lines = split( message, "\n", &numLines );
            
            if( numLines > 0 ) {
                // skip fist
                delete [] lines[0];
                }
            
            
            for( int i=1; i<numLines; i++ ) {
                
                int x, y, responsiblePlayerID;
                
                                
                char *idBuffer = new char[500];

                int numRead = sscanf( lines[i], "%d %d %499s %d",
                                      &x, &y, idBuffer, &responsiblePlayerID );
                if( numRead == 4 ) {
                    int mapX = x - mMapOffsetX + mMapD / 2;
                    int mapY = y - mMapOffsetY + mMapD / 2;
                    
                    if( mapX >= 0 && mapX < mMapD
                        &&
                        mapY >= 0 && mapY < mMapD ) {
                        
                        int mapI = mapY * mMapD + mapX;

                        int old = mMap[mapI];

                        if( strstr( idBuffer, "," ) != NULL ) {
                            int numInts;
                            char **ints = 
                                split( idBuffer, ",", &numInts );
                        
                            
                            
                            mMap[mapI] = atoi( ints[0] );
                            
                            delete [] ints[0];
                            
                            mMapContainedStacks[mapI].deleteAll();
                            
                            int numContained = numInts - 1;
                            
                            for( int c=0; c<numContained; c++ ) {
                                mMapContainedStacks[mapI].push_back(
                                    atoi( ints[ c + 1 ] ) );
                                delete [] ints[ c + 1 ];
                                }
                            delete [] ints;
                            }
                        else {
                            // a single int
                            mMap[mapI] = atoi( idBuffer );
                            mMapContainedStacks[mapI].deleteAll();
                            }

                        if( old == 0 && mMap[mapI] != 0 ) {
                            // new placement
                            
                            printf( "New placement, responsible=%d\n",
                                    responsiblePlayerID );

                            mMapAnimationFrameCount[mapI] = 0;
                            mMapCurAnimType[mapI] = ground;
                            mMapLastAnimType[mapI] = held;
                            mMapLastAnimFade[mapI] = 1;
                            
                            
                            // copy frame count from last holder
                            // of this object (server tracks
                            // who was holding it and tell us about it)
                            if( responsiblePlayerID != -1 ) {
                                for( int i=0; i<gameObjects.size(); i++ ) {
        
                                    LiveObject *nextObject =
                                        gameObjects.getElement( i );
                                    
                                    if( nextObject->id ==
                                        responsiblePlayerID ) {

                                        mMapAnimationFrameCount[mapI] =
                                            lrint( nextObject->
                                                   heldAnimationFrameCount );
                                        
                                        if( nextObject->lastAnimFade == 
                                            0 ) {
                                            
                                            mMapLastAnimType[mapI] = 
                                                nextObject->curHeldAnim;
                                            }
                                        else {
                                            // dropped object was already
                                            // in the middle of a fade
                                            mMapCurAnimType[mapI] =
                                                nextObject->curHeldAnim;
                                            
                                            mMapLastAnimType[mapI] =
                                                nextObject->lastHeldAnim;
                                            
                                            mMapLastAnimFade[mapI] =
                                                nextObject->lastHeldAnimFade;
                                            }
                                        
                                            
                                            
                                        mMapDropOffsets[mapI].x = 
                                            nextObject->heldObjectPos.x - x;
                                        
                                        mMapDropOffsets[mapI].y = 
                                            nextObject->heldObjectPos.y - y;
                                        
                                        mMapTileFlips[mapI] =
                                            nextObject->holdingFlip;
                                        
                                        if( mMapDropOffsets[mapI].x > 0 ) {
                                            nextObject->holdingFlip = false;
                                            }
                                        else if( 
                                            mMapDropOffsets[mapI].x < 0 ) {
                                            
                                            nextObject->holdingFlip = true;
                                            }
                                            
                                        break;
                                        }           
                                    }
                                }
                            }
                        }
                    }
                
                delete [] idBuffer;
                
                delete [] lines[i];
                }
            
            delete [] lines;
            }
        else if( type == PLAYER_UPDATE ) {
            
            int numLines;
            char **lines = split( message, "\n", &numLines );
            
            if( numLines > 0 ) {
                // skip fist
                delete [] lines[0];
                }
            
            
            for( int i=1; i<numLines; i++ ) {

                LiveObject o;
                
                o.pathToDest = NULL;
                o.containedIDs = NULL;


                // don't track these for other players
                o.foodStore = 0;
                o.foodCapacity = 0;
                
                o.currentSpeech = NULL;
                o.speechFade = 1.0;
                
                
                int forced = 0;
                
                
                char *holdingIDBuffer = new char[500];

                int heldOriginValid, heldOriginX, heldOriginY;

                int hat, tunic, frontShoe, backShoe;
                
                int numRead = sscanf( lines[i], 
                                      "%d %d "
                                      "%499s %d %d %d %f %d %d %d "
                                      "%lf %lf %lf %d,%d,%d,%d",
                                      &( o.id ),
                                      &( o.displayID ),
                                      holdingIDBuffer,
                                      &heldOriginValid,
                                      &heldOriginX,
                                      &heldOriginY,
                                      &( o.heat ),
                                      &forced,
                                      &( o.xd ),
                                      &( o.yd ),
                                      &( o.age ),
                                      &( o.ageRate ),
                                      &( o.lastSpeed ),
                                      &( hat ),
                                      &( tunic ),
                                      &( frontShoe ),
                                      &( backShoe ) );
                
                if( numRead == 17 ) {
                    o.lastAgeSetTime = game_getCurrentTime();

                    o.clothing.hat = getObject( hat );
                    o.clothing.tunic = getObject( tunic );
                    o.clothing.frontShoe = getObject( frontShoe );
                    o.clothing.backShoe = getObject( backShoe );
                    

                    if( strstr( holdingIDBuffer, "," ) != NULL ) {
                        int numInts;
                        char **ints = split( holdingIDBuffer, ",", &numInts );
                        
                        o.holdingID = atoi( ints[0] );
                        
                        delete [] ints[0];

                        o.numContained = numInts - 1;
                        o.containedIDs = new int[ o.numContained ];
                        
                        for( int c=0; c<o.numContained; c++ ) {
                            o.containedIDs[c] = atoi( ints[ c + 1 ] );
                            delete [] ints[ c + 1 ];
                            }
                        delete [] ints;
                        }
                    else {
                        // a single int
                        o.holdingID = atoi( holdingIDBuffer );
                        o.numContained = 0;
                        }
                    
                    
                    o.xServer = o.xd;
                    o.yServer = o.yd;

                    LiveObject *existing = NULL;

                    for( int j=0; j<gameObjects.size(); j++ ) {
                        if( gameObjects.getElement(j)->id == o.id ) {
                            existing = gameObjects.getElement(j);
                            break;
                            }
                        }
                    
                    if( existing != NULL ) {
                        int oldHeld = existing->holdingID;
                        
                        existing->holdingID = o.holdingID;
                        
                        if( existing->holdingID == 0 ) {
                                                        
                            // don't reset these when dropping something
                            // leave them in place so that dropped object
                            // can use them for smooth fade
                            // existing->animationFrameCount = 0;
                            // existing->curAnim = ground;
                            
                            //existing->lastAnim = ground;
                            //existing->lastAnimFade = 0;
                            }
                        else if( oldHeld == 0 ) {
                            // holding something new

                            // start new anim
                            existing->lastHeldAnim = ground;
                            existing->lastHeldAnimFade = 1;
                            existing->curHeldAnim = held;

                            existing->futureHeldAnimStack.deleteAll();

                            if( heldOriginValid ) {
                                // transition from last ground animation
                                // of object, keeping that frame count
                                // for smooth transition
                                
                                existing->heldPosOverride = true;
                                existing->heldObjectPos.x = heldOriginX;
                                existing->heldObjectPos.y = heldOriginY;
                                
                                int mapX = 
                                    heldOriginX - mMapOffsetX + mMapD / 2;
                                int mapY = 
                                    heldOriginY - mMapOffsetY + mMapD / 2;
                    
                                if( mapX >= 0 && mapX < mMapD
                                    &&
                                    mapY >= 0 && mapY < mMapD ) {
                        
                                    int mapI = mapY * mMapD + mapX;
                                
                                    existing->heldAnimationFrameCount =
                                        mMapAnimationFrameCount[ mapI ];
                                    
                                    if( mMapLastAnimFade[ mapI ] == 0 ) {
                                        existing->lastHeldAnim = 
                                            mMapCurAnimType[ mapI ];
                                        existing->lastHeldAnimFade = 1;
                                        existing->curHeldAnim = held;
                                        }
                                    else {
                                        // map spot is in the middle of
                                        // an animation fade
                                        existing->lastHeldAnim = 
                                            mMapLastAnimType[ mapI ];
                                        existing->curHeldAnim = 
                                            mMapCurAnimType[ mapI ];
                                        
                                        existing->lastHeldAnimFade =
                                            mMapLastAnimFade[ mapI ];
                                        
                                        existing->futureHeldAnimStack.
                                            push_back( held );
                                        }
                                    
                                    }
                                }
                            else {
                                existing->heldObjectPos = existing->currentPos;
                                }
                            
                            // otherwise, don't touch frame count
                            }
                        
                        existing->displayID = o.displayID;
                        existing->age = o.age;
                        existing->lastAgeSetTime = o.lastAgeSetTime;
                        
                        existing->heat = o.heat;

                        if( existing->containedIDs != NULL ) {
                            delete [] existing->containedIDs;
                            }
                        existing->containedIDs = o.containedIDs;
                        existing->numContained = o.numContained;
                        
                        existing->xServer = o.xServer;
                        existing->yServer = o.yServer;
                        
                        existing->lastSpeed = o.lastSpeed;
                        
                        existing->clothing = o.clothing;
                        

                        if( existing->id != ourID || 
                            forced ) {
                            
                            // don't ever force-update these for
                            // our locally-controlled object
                            // give illusion of it being totally responsive
                            // to move commands

                            // UNLESS server tells us to force update
                            existing->currentPos.x = o.xd;
                            existing->currentPos.y = o.yd;
                        
                            existing->currentSpeed = 0;
                            
                            existing->xd = o.xd;
                            existing->yd = o.yd;

                            addNewAnim( existing, held );
                            }
                        
                        if( existing->id == ourID ) {
                            // update for us
                            
                            if( !existing->inMotion ) {
                                // this is an update post-action, not post-move
                                
                                // ready to execute next action
                                playerActionPending = false;
                                playerActionTargetNotAdjacent = false;

                                existing->pendingAction = false;
                                }

                            if( forced ) {
                                existing->pendingActionAnimationProgress = 0;
                                existing->pendingAction = false;
                                
                                playerActionPending = false;
                                playerActionTargetNotAdjacent = false;

                                if( nextActionMessageToSend != NULL ) {
                                    delete [] nextActionMessageToSend;
                                    nextActionMessageToSend = NULL;
                                    }
                                }
                            }
                        
                        // in motion until update received, now done
                        existing->inMotion = false;
                        
                        existing->moveTotalTime = 0;
                        }
                    else {    
                        o.displayChar = lastCharUsed + 1;
                    
                        lastCharUsed = o.displayChar;
                    
                        o.curAnim = held;
                        o.lastAnim = held;
                        o.lastAnimFade = 0;

                        o.curHeldAnim = held;
                        o.lastHeldAnim = held;
                        o.lastHeldAnimFade = 0;
                        
                        o.heldAnimationFrameFrozen = false;
                        

                        o.inMotion = false;
                        
                        o.holdingFlip = false;

                        o.pendingAction = false;
                        o.pendingActionAnimationProgress = 0;
                        
                        o.currentPos.x = o.xd;
                        o.currentPos.y = o.yd;

                        o.heldPosOverride = false;
                        o.heldObjectPos = o.currentPos;
                        
                        o.currentSpeed = 0;
                                                
                        o.moveTotalTime = 0;
                        
                        
                        gameObjects.push_back( o );
                        }
                    }
                else if( strstr( lines[i], "X X" ) != NULL  ) {
                    // object deleted
                        
                    numRead = sscanf( lines[i], "%d %d",
                                      &( o.id ),
                                      &( o.holdingID ) );
                    
                    
                    for( int i=0; i<gameObjects.size(); i++ ) {
                        
                        LiveObject *nextObject =
                            gameObjects.getElement( i );
                        
                        if( nextObject->id == o.id ) {
                            
                            if( nextObject->pathToDest != NULL ) {
                                delete [] nextObject->pathToDest;
                                }
                            
                            gameObjects.deleteElement( i );
                            break;
                            }
                        }
                    }
                
                delete [] holdingIDBuffer;
                
                delete [] lines[i];
                }
            

            delete [] lines;


            if( ( mFirstServerMessagesReceived & 2 ) == 0 ) {
            
                LiveObject *ourObject = 
                    gameObjects.getElement( gameObjects.size() - 1 );
                
                ourID = ourObject->id;
                
                ourObject->displayChar = 'A';

                // center view on player's starting position
                lastScreenViewCenter.x = CELL_D * ourObject->xd;
                lastScreenViewCenter.y = CELL_D * ourObject->yd;

                setViewCenterPosition( lastScreenViewCenter.x, 
                                       lastScreenViewCenter.y );
                }
            
            mFirstServerMessagesReceived |= 2;
            }
        else if( type == PLAYER_MOVES_START ) {
            
            int numLines;
            char **lines = split( message, "\n", &numLines );
            
            if( numLines > 0 ) {
                // skip fist
                delete [] lines[0];
                }
            
            
            for( int i=1; i<numLines; i++ ) {

                LiveObject o;

                double etaSec;
                
                int startX, startY;
                
                int truncated;
                
                int numRead = sscanf( lines[i], "%d %d %d %lf %lf %d",
                                      &( o.id ),
                                      &( startX ),
                                      &( startY ),
                                      &( o.moveTotalTime ),
                                      &etaSec,
                                      &truncated );

                SimpleVector<char *> *tokens =
                    tokenizeString( lines[i] );
        

                o.pathLength = 0;
                o.pathToDest = NULL;
                
                // require an even number at least 8
                if( tokens->size() < 8 || tokens->size() % 2 != 0 ) {
                    }
                else {                    
                    int numTokens = tokens->size();
        
                    o.pathLength = (numTokens - 6) / 2 + 1;
        
                    o.pathToDest = new GridPos[ o.pathLength ];

                    o.pathToDest[0].x = startX;
                    o.pathToDest[0].y = startY;

                    for( int e=1; e<o.pathLength; e++ ) {
            
                        char *xToken = 
                            tokens->getElementDirect( 6 + (e-1) * 2 );
                        char *yToken = 
                            tokens->getElementDirect( 6 + (e-1) * 2 + 1 );
                        
        
                        sscanf( xToken, "%d", &( o.pathToDest[e].x ) );
                        sscanf( yToken, "%d", &( o.pathToDest[e].y ) );
                        
                        // make them absolute
                        o.pathToDest[e].x += startX;
                        o.pathToDest[e].y += startY;
                        }
        
                    }

                tokens->deallocateStringElements();
                delete tokens;
                    
                
                
                
                o.moveEtaTime = etaSec + game_getCurrentTime();
                

                if( numRead == 6 && o.pathLength > 0 ) {
                
                    o.xd = o.pathToDest[ o.pathLength -1 ].x;
                    o.yd = o.pathToDest[ o.pathLength -1 ].y;
                
    
                    for( int j=0; j<gameObjects.size(); j++ ) {
                        if( gameObjects.getElement(j)->id == o.id ) {
                            
                            LiveObject *existing = gameObjects.getElement(j);
                            
                            double timePassed = 
                                o.moveTotalTime - etaSec;
                            
                            double fractionPassed = 
                                timePassed / o.moveTotalTime;
                            
                            
                            // stays in motion until we receive final
                            // PLAYER_UPDATE from server telling us
                            // that move is over
                            existing->inMotion = true;
                            
                            int oldPathLength = 0;
                            GridPos oldCurrentPathPos;
                            
                            if( existing->currentSpeed != 0
                                &&
                                existing->pathToDest != NULL ) {
                                
                                // an interrupted move
                                oldPathLength = existing->pathLength;
                                oldCurrentPathPos = 
                                    existing->pathToDest[
                                        existing->currentPathStep ];
                                }
                            

                            if( existing->id != ourID ) {
                                // remove any double-backs from path
                                // because they confuse smooth path following
                                
                                SimpleVector<GridPos> filteredPath;
                                
                                int dbA = -1;
                                int dbB = -1;
                                
                                int longestDB = 0;
                                
                                
                                for( int e=0; e<o.pathLength; e++ ) {
                                    
                                    for( int f=e; f<o.pathLength; f++ ) {
                                        
                                        if( equal( o.pathToDest[e],
                                                   o.pathToDest[f] ) ) {
                                            
                                            int dist = f - e;
                                            
                                            if( dist > longestDB ) {
                                                
                                                dbA = e;
                                                dbB = f;
                                                longestDB = dist;
                                                }
                                            }
                                        }
                                    }
                                
                                if( longestDB > 0 ) {
                                    
                                    printf( "Removing loop with %d elements\n",
                                            longestDB );

                                    for( int e=0; e<=dbA; e++ ) {
                                        filteredPath.push_back( 
                                            o.pathToDest[e] );
                                        }
                                    
                                    // skip loop between

                                    for( int e=dbB + 1; e<o.pathLength; e++ ) {
                                        filteredPath.push_back( 
                                            o.pathToDest[e] );
                                        }
                                    
                                    o.pathLength = filteredPath.size();
                                    
                                    delete [] o.pathToDest;
                                    
                                    o.pathToDest = 
                                        filteredPath.getElementArray();
                                    }
                                
                                    
                                }
                            



                            if( existing->id != ourID ||
                                truncated ) {
                                // always replace path for other players
                                // with latest from server

                                // only replace OUR path if we
                                // learn that a path we submitted
                                // was truncated

                                existing->pathLength = o.pathLength;

                                if( existing->pathToDest != NULL ) {
                                    delete [] existing->pathToDest;
                                    }
                                existing->pathToDest = 
                                    new GridPos[ o.pathLength ];
                                
                                memcpy( existing->pathToDest,
                                        o.pathToDest,
                                        sizeof( GridPos ) * o.pathLength );

                                existing->xd = o.xd;
                                existing->yd = o.yd;
                                }
                            



                            if( existing->id != ourID ) {
                                // don't force-update these
                                // for our object
                                // we control it locally, to keep
                                // illusion of full move interactivity
                            
                                char usingOldPathStep = false;

                                if( oldPathLength != 0 ) {
                                    // this move interrupts or truncates
                                    // the move we were already on

                                    // look to see if the old path step
                                    // is on our new path
                                    char found = false;
                                    int foundStep = -1;

                                    for( int p=0; 
                                         p<existing->pathLength - 1;
                                         p++ ) {
                                        
                                        if( equal( existing->pathToDest[p],
                                                   oldCurrentPathPos ) ) {
                                            
                                            found = true;
                                            foundStep = p;
                                            }
                                        }
                                    
                                    if( found ) {
                                        usingOldPathStep = true;
                                        
                                        existing->currentPathStep =
                                            foundStep;
                                        
                                        doublePair foundWorld = 
                                            gridToDouble(
                                                existing->
                                                pathToDest[ foundStep ] );

                                        // where we should move toward
                                        doublePair nextWorld;
                                        
                                        if( foundStep < 
                                            existing->pathLength - 1 ) {
                                            
                                            // point from here to our
                                            // next step
                                            nextWorld = 
                                                gridToDouble(
                                                    existing->
                                                    pathToDest[ 
                                                        foundStep + 1 ] );    
                                            }
                                        else {
                                            // at end of path, point right
                                            // to it
                                            nextWorld = foundWorld;
                                            }

                                        existing->currentMoveDirection =
                                            normalize( 
                                                sub( nextWorld, 
                                                     existing->currentPos ) );
                                        }
                                    }
                                
                                
                                if( ! usingOldPathStep ) {
                                    // forced to jump to exactly where
                                    // server says we are
                                    
                                    // current step
                                    int b = 
                                        (int)floor( fractionPassed * 
                                                    existing->pathLength );
                                    // next step
                                    int n =
                                        (int)ceil( fractionPassed *
                                                   existing->pathLength );
                                    
                                    if( n == b ) {
                                        if( n < existing->pathLength - 1 ) {
                                            n ++ ;
                                            }
                                        else {
                                            b--;
                                            }
                                        }
                                    
                                    existing->currentPathStep = b;
                                
                                    double nWeight =
                                        fractionPassed * existing->pathLength 
                                        - b;
                                
                                    doublePair bWorld =
                                        gridToDouble(
                                            existing->pathToDest[ b ] );
                                    
                                    doublePair nWorld =
                                        gridToDouble(
                                            existing->pathToDest[ n ] );
                                    
                                
                                    existing->currentPos =
                                        add( mult( bWorld, 1 - nWeight ), 
                                             mult( nWorld, nWeight ) );
                                    
                                    existing->currentMoveDirection =
                                        normalize( sub( nWorld, bWorld ) );
                                    }
                                
                                
                                existing->moveTotalTime = o.moveTotalTime;
                                existing->moveEtaTime = o.moveEtaTime;

                                if( usingOldPathStep ) {
                                    // we're ignoring where server
                                    // says we should be

                                    // BUT, we should change speed to
                                    // compensate for the difference

                                    double oldFractionPassed =
                                        (double)existing->currentPathStep
                                        / (double)existing->pathLength;
                                    
                                    
                                    // if this is positive, we are
                                    // farther along than we should be
                                    // we need to slow down (moveEtaTime
                                    // should get bigger)

                                    // if negative, we are behind
                                    // we should speed up (moveEtaTime
                                    // should get smaller)
                                    double fractionDiff =
                                        oldFractionPassed - fractionPassed;
                                    
                                    double timeAdjust =
                                        existing->moveTotalTime * fractionDiff;
                                    
                                    existing->moveEtaTime += timeAdjust;
                                    }
                                

                                updateMoveSpeed( existing );
                                }
                            else if( truncated ) {
                                // adjustment to our own movement
                                
                                // cancel pending action upon arrival
                                // (no longer possible, since truncated)

                                existing->pendingActionAnimationProgress = 0;
                                existing->pendingAction = false;
                                
                                playerActionPending = false;
                                playerActionTargetNotAdjacent = false;

                                if( nextActionMessageToSend != NULL ) {
                                    delete [] nextActionMessageToSend;
                                    nextActionMessageToSend = NULL;
                                    }

                                // this path may be different
                                // from what we actually requested
                                // from sever

                                char stillOnPath = false;
                                    
                                if( oldPathLength > 0 ) {
                                    // on a path, perhaps some other one
                                    
                                    // check if our current pos
                                    // is on this new, truncated path
                                    
                                    char found = false;
                                    int foundStep = -1;
                                    for( int p=0; 
                                         p<existing->pathLength - 1;
                                         p++ ) {
                                        
                                        if( equal( existing->pathToDest[p],
                                                   oldCurrentPathPos ) ) {
                                            
                                            found = true;
                                            foundStep = p;
                                            }
                                        }
                                    
                                        if( found ) {
                                            stillOnPath = true;
                                            
                                            existing->currentPathStep =
                                                foundStep;
                                            }
                                        
                                    }
                                    


                                // only jump around if we must

                                if( ! stillOnPath ) {
                                    // path truncation from what we last knew
                                    // for ourselves, and we're off the end
                                    // of the new path
                                    
                                    // hard jump back
                                    existing->currentSpeed = 0;
                                    
                                    existing->currentPos.x =
                                        existing->xd;
                                    existing->currentPos.y =
                                        existing->yd;
                                    }
                                }
                            
                            

                            

                            break;
                            }
                        }
                    }
                
                if( o.pathToDest != NULL ) {
                    delete [] o.pathToDest;
                    }

                delete [] lines[i];
                }
            

            delete [] lines;
            }
        else if( type == PLAYER_SAYS ) {
            int numLines;
            char **lines = split( message, "\n", &numLines );
            
            if( numLines > 0 ) {
                // skip first
                delete [] lines[0];
                }
            
            
            for( int i=1; i<numLines; i++ ) {

                int id;
                int numRead = sscanf( lines[i], "%d ",
                                      &( id ) );

                if( numRead == 1 ) {
                    for( int j=0; j<gameObjects.size(); j++ ) {
                        if( gameObjects.getElement(j)->id == id ) {
                            
                            LiveObject *existing = gameObjects.getElement(j);
                            
                            if( existing->currentSpeech != NULL ) {
                                delete [] existing->currentSpeech;
                                existing->currentSpeech = NULL;
                                }
                            
                            char *firstSpace = strstr( lines[i], " " );
        
                            if( firstSpace != NULL ) {
                                existing->currentSpeech = 
                                    stringDuplicate( &( firstSpace[1] ) );
                                
                                existing->speechFade = 1.0;
                                
                                // longer time for longer speech
                                existing->speechFadeETATime = 
                                    game_getCurrentTime() + 3 +
                                    strlen( existing->currentSpeech ) / 5;
                                }
                            
                            break;
                            }
                        }
                    
                    }
                
                delete [] lines[i];
                }
            delete [] lines;
            }
        else if( type == FOOD_CHANGE ) {
            
            LiveObject *ourLiveObject = getOurLiveObject();
            
            if( ourLiveObject != NULL ) {
                
                sscanf( message, "FX\n%d %d %lf", 
                        &( ourLiveObject->foodStore ),
                        &( ourLiveObject->foodCapacity ),
                        &( ourLiveObject->lastSpeed ) );

                printf( "Our food = %d/%d\n", 
                        ourLiveObject->foodStore,
                        ourLiveObject->foodCapacity );
                }
            }
        
        

        delete [] message;

        // process next message if there is one
        message = getNextServerMessage();
        }
    
        
    // check if we're about to move off the screen
    LiveObject *ourLiveObject = NULL;

    for( int i=0; i<gameObjects.size(); i++ ) {
        
        LiveObject *o = gameObjects.getElement( i );
        
        if( o->id == ourID ) {
            ourLiveObject = o;
            break;
            }
        }

    
    if( ourLiveObject != NULL ) {
        

        // current age
        double age = 
            ourLiveObject->age + 
            ourLiveObject->ageRate * 
            ( game_getCurrentTime() - ourLiveObject->lastAgeSetTime );

        int sayCap = (int)( floor( age ) + 1 );
        
        mSayField.setMaxLength( sayCap );

        doublePair screenTargetPos = mult( ourLiveObject->currentPos, CELL_D );
        

        if( ourLiveObject->currentPos.x != ourLiveObject->xd
            ||
            ourLiveObject->currentPos.y != ourLiveObject->yd ) {
            
            // moving

            // push camera out in front
            
            doublePair farthestPathPos;
            
            farthestPathPos.x = (double)ourLiveObject->xd;
            farthestPathPos.y = (double)ourLiveObject->yd;
            
            doublePair moveDir = sub( farthestPathPos, 
                                      ourLiveObject->currentPos );
                        
            
            if( length( moveDir ) > 7 ) {
                moveDir = mult( normalize( moveDir ), 7 );
                }
                
            moveDir = mult( moveDir, CELL_D );
            
            
            screenTargetPos = add( screenTargetPos, moveDir );
            }
        
        // whole pixels
        screenTargetPos.x = round( screenTargetPos.x );
        screenTargetPos.y = round( screenTargetPos.y );
        

        doublePair dir = sub( screenTargetPos, lastScreenViewCenter );
        
        char viewChange = false;
        
        int maxR = 50;
        double moveSpeedFactor = .5;
        

        if( length( dir ) > maxR ) {
            
            double moveScale = moveSpeedFactor * sqrt( length( dir ) - maxR ) 
                * frameRateFactor;

            doublePair moveStep = mult( normalize( dir ), moveScale );
            
            // whole pixels

            moveStep.x = lrint( moveStep.x );
            moveStep.y = lrint( moveStep.y );
                        
            if( length( moveStep ) > 0 ) {
                lastScreenViewCenter = add( lastScreenViewCenter, 
                                            moveStep );
                viewChange = true;
                }
            }
        

        if( viewChange ) {
            
            setViewCenterPosition( lastScreenViewCenter.x, 
                                   lastScreenViewCenter.y );
            
            }

        }
    
    
    // update all positions for moving objects
    for( int i=0; i<gameObjects.size(); i++ ) {
        
        LiveObject *o = gameObjects.getElement( i );
        

        if( o->currentSpeech != NULL ) {
            if( game_getCurrentTime() > o->speechFadeETATime ) {
                
                o->speechFade -= 0.05 * frameRateFactor;

                if( o->speechFade <= 0 ) {
                    delete [] o->currentSpeech;
                    o->currentSpeech = NULL;
                    o->speechFade = 1.0;
                    }
                }
            }
        


        if( o->lastAnimFade > 0 ) {
            // constant anim speed during anim fades
            o->animationFrameCount++;
            }
        else {
            o->animationFrameCount += o->lastSpeed / BASE_SPEED;
            }
            
        if( o->lastAnimFade > 0 ) {
            
            if( o->lastAnimFade == 1 ) {
                // fade just started
                // check if it's necessary
                
                if( isAnimFadeNeeded( o->displayID, 
                                      o->lastAnim, o->curAnim ) ) {
                    // fade needed, do nothing
                    }
                else {
                    // fade not needed
                    // jump to end of it
                    o->lastAnimFade = 0;
                    }
                }
            

            o->lastAnimFade -= 0.05 * frameRateFactor;
            if( o->lastAnimFade < 0 ) {
                o->lastAnimFade = 0;
                // start current animation fresh after fade
                o->animationFrameCount = 0;
                o->animationFrozenRotFrameCount = 0;
                
                if( o->futureAnimStack.size() > 0 ) {
                    // move on to next in stack
                    o->lastAnim = o->curAnim;
                    
                    o->lastAnimFade = 1;
                    
                    // pop from stack
                    o->curAnim = 
                        o->futureAnimStack.getElementDirect( 0 );
                    o->futureAnimStack.deleteElement( 0 );
                    }
                
                }
            }


        if( ! o->heldAnimationFrameFrozen ) {

            if( o->lastHeldAnimFade > 0 ) {
                // constant anim speed during anim fades
                o->heldAnimationFrameCount++;
                }
            else {
                o->heldAnimationFrameCount += o->lastSpeed / BASE_SPEED;
                }
            }
        
        if( o->lastHeldAnimFade > 0 ) {
            
            if( o->lastHeldAnimFade == 1 ) {
                // fade just started
                // check if it's necessary
                
                if( o->holdingID != 0 &&
                    isAnimFadeNeeded( o->holdingID, 
                                      o->lastHeldAnim, o->curHeldAnim ) ) {
                    // fade needed, do nothing
                    }
                else {
                    // fade not needed
                    // jump to end of it
                    o->lastHeldAnimFade = 0;
                    }
                }
            

            o->lastHeldAnimFade -= 0.05 * frameRateFactor;
            if( o->lastHeldAnimFade < 0 ) {
                o->lastHeldAnimFade = 0;
                // start current animation fresh after fade
                o->heldAnimationFrameCount = 0;
                o->heldAnimationFrozenRotFrameCount = 0;

                if( o->futureHeldAnimStack.size() > 0 ) {
                    // move on to next in stack
                    o->lastHeldAnim = o->curHeldAnim;
                    
                    o->lastHeldAnimFade = 1;
                    
                    // pop from stack
                    o->curHeldAnim = 
                        o->futureHeldAnimStack.getElementDirect( 0 );
                    o->futureHeldAnimStack.deleteElement( 0 );
                    }
                
                }
            }


        if( o->currentSpeed != 0 ) {

            GridPos curStepDest = o->pathToDest[ o->currentPathStep ];
            GridPos nextStepDest = o->pathToDest[ o->currentPathStep + 1 ];
            
            doublePair startPos = { (double)curStepDest.x, 
                                    (double)curStepDest.y };

            doublePair endPos = { (double)nextStepDest.x, 
                                  (double)nextStepDest.y };
            
            doublePair dir = normalize( sub( endPos, o->currentPos ) );
            

            double turnFactor = 0.35;
            
            if( o->currentPathStep == o->pathLength - 2 ) {
                // last segment
                // speed up turn toward final spot so that we
                // don't miss it and circle around it forever
                turnFactor = 0.5;
                }
            
            
            o->currentMoveDirection = 
                normalize( add( o->currentMoveDirection, 
                                mult( dir, turnFactor * frameRateFactor ) ) );

            // don't change flip unless moving in x
            if( o->currentMoveDirection.x != 0 ) {
                
                if( o->currentMoveDirection.x > 0 ) {
                    o->holdingFlip = false;
                    }
                else {
                    o->holdingFlip = true;
                    }         
                }
            

            if( o->currentPathStep < o->pathLength - 2 ) {

                o->currentPos = add( o->currentPos,
                                     mult( o->currentMoveDirection,
                                           o->currentSpeed ) );
                
                addNewAnim( o, moving );
                
                
                if( 1.5 * distance( o->currentPos,
                                    startPos )
                    >
                    distance( o->currentPos,
                              endPos ) ) {
                    
                    o->currentPathStep ++; 
                    }
                }
            else {
                if( distance( endPos, o->currentPos )
                    < o->currentSpeed ) {

                    // reached destination
                    o->currentPos = endPos;
                    o->currentSpeed = 0;


                    addNewAnim( o, held );
                                        

                    printf( "Reached dest %f seconds early\n",
                            o->moveEtaTime - game_getCurrentTime() );
                    }
                else {

                    addNewAnim( o, moving );
                    
                    o->currentPos = add( o->currentPos,
                                         mult( o->currentMoveDirection,
                                               o->currentSpeed ) );
                    }
                }
            
            // correct move speed based on how far we have left to go
            // and eta wall-clock time
            
            // make this correction once per second
            if( game_getCurrentTime() - o->timeOfLastSpeedUpdate
                > .25 ) {
    
                //updateMoveSpeed( o );
                }
            
            }

        if( o->id == ourID &&
            ( o->pendingAction || o->pendingActionAnimationProgress != 0 ) ) {
            
            o->pendingActionAnimationProgress += 0.025 * frameRateFactor;
            
            if( o->pendingActionAnimationProgress > 1 ) {
                if( o->pendingAction ) {
                    // still pending, wrap around smoothly
                    o->pendingActionAnimationProgress -= 1;
                    }
                else {
                    // no longer pending, finish last cycle by snapping
                    // back to 0
                    o->pendingActionAnimationProgress = 0;
                    }
                }
            }
        }
    

    if( nextActionMessageToSend != NULL 
        && ourLiveObject->currentSpeed == 0 
        && (
            isGridAdjacent( ourLiveObject->xd, ourLiveObject->yd,
                            playerActionTargetX, playerActionTargetY )
            ||
            ( ourLiveObject->xd == playerActionTargetX &&
              ourLiveObject->yd == playerActionTargetY ) 
            ||
            playerActionTargetNotAdjacent ) ) {
        
        // done moving on client end
        // can start showing pending action animation, even if 
        // end of motion not received from server yet

        if( !playerActionPending ) {
            playerActionPending = true;
            ourLiveObject->pendingAction = true;
            
            // start on first frame to force at least one cycle no
            // matter how fast the server responds
            ourLiveObject->pendingActionAnimationProgress = 
                0.025 * frameRateFactor;
            }
        
        
        // wait until 
        // we've stopped moving locally
        // AND animation has played for a bit
        // AND server agrees with our position
        if( ! ourLiveObject->inMotion && 
            ourLiveObject->pendingActionAnimationProgress > 0.25 &&
            ourLiveObject->xd == ourLiveObject->xServer &&
            ourLiveObject->yd == ourLiveObject->yServer ) {
            
            printf( "Sending pending action message to server: %s\n",
                    nextActionMessageToSend );
            
            // move end acked by server AND action animation in progress

            // queued action waiting for our move to end
            sendToSocket( mServerSocket, 
                          (unsigned char*)nextActionMessageToSend, 
                          strlen( nextActionMessageToSend) );
            
            numServerBytesSent += strlen( nextActionMessageToSend );
            overheadServerBytesSent += 52;

            delete [] nextActionMessageToSend;
            nextActionMessageToSend = NULL;
            }
        }



    if( mLastMouseOverFade != 0 ) {
        mLastMouseOverFade -= 0.05 * frameRateFactor;
        if( mLastMouseOverFade < 0 ) {
            mLastMouseOverFade = 0;
            mLastMouseOverID = 0;
            }
        }
    
    }


  
void LivingLifePage::makeActive( char inFresh ) {
    // unhold E key
    mEKeyDown = false;

    if( !inFresh ) {
        return;
        }
    
    lastScreenViewCenter.x = 0;
    lastScreenViewCenter.y = 0;
    
    setViewCenterPosition( lastScreenViewCenter.x, lastScreenViewCenter.y );
    
    mPageStartTime = game_getCurrentTime();
    
    setWaiting( true, false );

    clearLiveObjects();
    mFirstServerMessagesReceived = 0;
    
    serverSocketBuffer.deleteAll();
    }



        





void LivingLifePage::pointerMove( float inX, float inY ) {
    int clickDestX = lrintf( ( inX ) / CELL_D );
    
    int clickDestY = lrintf( ( inY ) / CELL_D );
    
    int destID = 0;
        
    int mapX = clickDestX - mMapOffsetX + mMapD / 2;
    int mapY = clickDestY - mMapOffsetY + mMapD / 2;
    if( mapY >= 0 && mapY < mMapD &&
        mapX >= 0 && mapX < mMapD ) {
        
        destID = mMap[ mapY * mMapD + mapX ];
        }

    if( destID > 0 ) {
        mCurMouseOverID = destID;
        
        }
    else if( mCurMouseOverID != 0 ) {
        mLastMouseOverID = mCurMouseOverID;
        mLastMouseOverFade = 1.0;
        mCurMouseOverID = 0;
        }
    
    }







void LivingLifePage::pointerDown( float inX, float inY ) {
    
    if( mFirstServerMessagesReceived != 3 ) {
        return;
        }

    if( playerActionPending ) {
        // block further actions until update received to confirm last
        // action
        return;
        }
    

    LiveObject *ourLiveObject;

    for( int i=0; i<gameObjects.size(); i++ ) {
        
        LiveObject *o = gameObjects.getElement( i );
        
        if( o->id == ourID ) {
            ourLiveObject = o;
            break;
            }
        }
    
    int clickDestX = lrintf( ( inX ) / CELL_D );
    
    int clickDestY = lrintf( ( inY ) / CELL_D );
    

    char modClick = false;
    
    if( mEKeyDown || isLastMouseButtonRight() ) {
        modClick = true;
        }

    if( clickDestX == ourLiveObject->xd && clickDestY == ourLiveObject->yd ) {
        // ignore clicks where we're already standing

        // unless it's a use-on-self action and standing still

        if( ! ourLiveObject->inMotion && ! modClick ) {
            
            if( nextActionMessageToSend != NULL ) {
                delete [] nextActionMessageToSend;
                nextActionMessageToSend = NULL;
                }
            
            nextActionMessageToSend = 
                autoSprintf( "USE %d %d#",
                             clickDestX, clickDestY );
            playerActionTargetX = clickDestX;
            playerActionTargetY = clickDestY;
            printf( "Use on self\n" );
            }
        

        return;
        }
    
    
    // may change to empty adjacent spot to click
    int moveDestX = clickDestX;
    int moveDestY = clickDestY;
    
    char mustMove = false;


    int destID = 0;
        
    int destNumContained = 0;
    
    int mapX = clickDestX - mMapOffsetX + mMapD / 2;
    int mapY = clickDestY - mMapOffsetY + mMapD / 2;
    
    printf( "clickDestX,Y = %d, %d,  mapX,Y = %d, %d, curX,Y = %d, %d\n", 
            clickDestX, clickDestY, 
            mapX, mapY,
            ourLiveObject->xd, ourLiveObject->yd );
    if( mapY >= 0 && mapY < mMapD &&
        mapX >= 0 && mapX < mMapD ) {
        
        destID = mMap[ mapY * mMapD + mapX ];
    
        destNumContained = mMapContainedStacks[ mapY * mMapD + mapX ].size();
        }
    
    printf( "DestID = %d\n", destID );
        

    if( nextActionMessageToSend != NULL ) {
        delete [] nextActionMessageToSend;
        nextActionMessageToSend = NULL;
        }
    

    // true if we're too far away to kill BUT we should execute
    // kill once we get to destination

    // if we're close enough to kill, we'll kill from where we're standing
    // and return
    char killLater = false;
    

    if( destID == 0 &&
        modClick && ourLiveObject->holdingID != 0 &&
        getObject( ourLiveObject->holdingID )->deadlyDistance > 0 ) {
        
        // special case

        // check for possible kill attempt at a distance

        // if it fails (target too far away or no person near),
        // then we resort to standard drop code below


        double d = sqrt( ( clickDestX - ourLiveObject->xd ) * 
                         ( clickDestX - ourLiveObject->xd )
                         +
                         ( clickDestY - ourLiveObject->yd ) * 
                         ( clickDestY - ourLiveObject->yd ) );

        doublePair targetPos = { clickDestX, clickDestY };
        


        for( int i=0; i<gameObjects.size(); i++ ) {
        
            LiveObject *o = gameObjects.getElement( i );
            
            if( o->id != ourID ) {
                if( distance( targetPos, o->currentPos ) < 1 ) {
                    // clicked on someone
                    
                    if( getObject( ourLiveObject->holdingID )->deadlyDistance 
                        >= d ) {
                        // close enough to use deadly object right now

                        
                        if( nextActionMessageToSend != NULL ) {
                            delete [] nextActionMessageToSend;
                            nextActionMessageToSend = NULL;
                            }
            
                        nextActionMessageToSend = 
                            autoSprintf( "KILL %d %d#",
                                         clickDestX, clickDestY );
                        playerActionTargetX = clickDestX;
                        playerActionTargetY = clickDestY;
                        
                        playerActionTargetNotAdjacent = true;
                        
                        printf( "KILL with target player %d\n", o->id );

                        return;
                        }
                    else {
                        // too far away, but try to kill later,
                        // once we walk there, using standard path-to-adjacent
                        // code below
                        killLater = true;
                        
                        break;
                        }
                    }
                }
            }
    
        }
    

    

    if( destID == 0 && !modClick ) {
        // a move to an empty spot
        // can interrupt current move
        
        mustMove = true;
        }
    else if( ( modClick && ourLiveObject->holdingID != 0 )
             || ( ! modClick && destID != 0 )
             || ( modClick && ourLiveObject->holdingID == 0 &&
                  destNumContained > 0 ) ) {
        // use/drop modifier
        // OR pick up action
            
        
        char canExecute = false;
        
        // direct click on adjacent cells?
        if( isGridAdjacent( clickDestX, clickDestY,
                            ourLiveObject->xd, ourLiveObject->yd ) ) {
            
            canExecute = true;
            }
        else {
            // need to move to empty adjacent first, if it exists
            
            
            int nDX[4] = { -1, +1, 0, 0 };
            int nDY[4] = { 0, 0, -1, +1 };
            
            char foundEmpty = false;
            
            int closestDist = 9999999;

            char oldPathExists = ( ourLiveObject->pathToDest != NULL );

            for( int n=0; n<4; n++ ) {
                int x = mapX + nDX[n];
                int y = mapY + nDY[n];

                if( y >= 0 && y < mMapD &&
                    x >= 0 && x < mMapD ) {
                 
                    
                    if( mMap[ y * mMapD + x ] == 0 ) {
                        
                        int emptyX = clickDestX + nDX[n];
                        int emptyY = clickDestY + nDY[n];

                        // check if there's a path there
                        int oldXD = ourLiveObject->xd;
                        int oldYD = ourLiveObject->yd;
                        
                        // set this temporarily for pathfinding
                        ourLiveObject->xd = emptyX;
                        ourLiveObject->yd = emptyY;
                        
                        computePathToDest( ourLiveObject );
                        
                        if( ourLiveObject->pathToDest != NULL &&
                            ourLiveObject->pathLength < closestDist ) {
                            
                            // can get there
                            
                            moveDestX = emptyX;
                            moveDestY = emptyY;
                            
                            closestDist = ourLiveObject->pathLength;
                            
                            foundEmpty = true;
                            }
                        
                        // restore our old dest
                        ourLiveObject->xd = oldXD;
                        ourLiveObject->yd = oldYD;    
                        }
                    
                    }
                }
            
            if( oldPathExists ) {
                // restore it
                computePathToDest( ourLiveObject );
                }

            if( foundEmpty ) {
                canExecute = true;
                mustMove = true;
                }
            }
        
        if( canExecute ) {
            
            const char *action = "";
            
            char send = false;
            
                
            if( modClick && destID == 0 && ourLiveObject->holdingID != 0 ) {
                action = "DROP";
                send = true;

                // special case:  we're too far away to kill someone
                // but we've right clicked on them from a distance
                // walk up and execute KILL once we get there.
                
                if( killLater ) {
                    action = "KILL";
                    }
                }
            else if( modClick && ourLiveObject->holdingID == 0 &&
                     destID != 0 &&
                     getNumContainerSlots( destID ) > 0 ) {
                action = "REMV";
                send = true;
                }
            else if( modClick && ourLiveObject->holdingID != 0 &&
                     destID != 0 &&
                     getNumContainerSlots( destID ) > 0 &&
                     destNumContained < getNumContainerSlots( destID ) ) {
                action = "DROP";
                send = true;
                }
            else if( ! modClick && destID != 0 ) {
                action = "USE";
                send = true;
                }
            
            
            if( send ) {
                // queue this until after we are done moving, if we are
                nextActionMessageToSend = 
                    autoSprintf( "%s %d %d#", action,
                                 clickDestX, clickDestY );
                playerActionTargetX = clickDestX;
                playerActionTargetY = clickDestY;
                }
            }
        }
    


    
    if( mustMove ) {
        
        int oldXD = ourLiveObject->xd;
        int oldYD = ourLiveObject->yd;
        
        ourLiveObject->xd = moveDestX;
        ourLiveObject->yd = moveDestY;
        
        ourLiveObject->inMotion = true;

        computePathToDest( ourLiveObject );

        
        if( ourLiveObject->pathToDest == NULL ) {
            // adjust move to closest possible
            ourLiveObject->xd = ourLiveObject->closestDestIfPathFailedX;
            ourLiveObject->yd = ourLiveObject->closestDestIfPathFailedY;

            computePathToDest( ourLiveObject );
            
            if( ourLiveObject->pathToDest == NULL ) {
                // this happens when our curPos is slightly off of xd,yd
                // but not a full cell away

                // make a fake path
                doublePair dest = { (double) ourLiveObject->xd,
                                    (double) ourLiveObject->yd };
                
                doublePair dir = normalize( sub( dest, 
                                                 ourLiveObject->currentPos ) );

                // fake start, one grid step away
                doublePair fakeStart = dest;

                if( fabs( dir.x ) > fabs( dir.y ) ) {
                    
                    if( dir.x < 0 ) {
                        fakeStart.x += 1;
                        }
                    else {
                        fakeStart.x -= 1;
                        }
                    }
                else {
                    if( dir.y < 0 ) {
                        fakeStart.y += 1;
                        }
                    else {
                        fakeStart.y -= 1;
                        }
                    
                    }
                
                
                ourLiveObject->pathToDest = new GridPos[2];
                
                ourLiveObject->pathToDest[0].x = (int)fakeStart.x;
                ourLiveObject->pathToDest[0].y = (int)fakeStart.y;
                
                ourLiveObject->pathToDest[1].x = ourLiveObject->xd;
                ourLiveObject->pathToDest[1].y = ourLiveObject->yd;
                
                ourLiveObject->pathLength = 2;
                ourLiveObject->currentPathStep = 0;
                
                ourLiveObject->currentMoveDirection =
                    normalize( 
                        sub( gridToDouble( ourLiveObject->pathToDest[1] ), 
                             gridToDouble( ourLiveObject->pathToDest[0] ) ) );
                }

            if( ourLiveObject->xd == oldXD 
                &&
                ourLiveObject->yd == oldYD ) {
                
                // truncated path is where we're already going
                // don't send new move message
                return;
                }


            moveDestX = ourLiveObject->xd;
            moveDestY = ourLiveObject->yd;
            
            if( nextActionMessageToSend != NULL ) {
                // abort the action, because we can't reach the spot we
                // want to reach
                delete [] nextActionMessageToSend;
                nextActionMessageToSend = NULL;
                }
            
            }
        

        // send move right away
        //Thread::staticSleep( 2000 );
        SimpleVector<char> moveMessageBuffer;
        
        moveMessageBuffer.appendElementString( "MOVE" );
        // start is absolute
        char *startString = autoSprintf( " %d %d", 
                                         ourLiveObject->pathToDest[0].x,
                                         ourLiveObject->pathToDest[0].y );
        moveMessageBuffer.appendElementString( startString );
        delete [] startString;
        
        for( int i=1; i<ourLiveObject->pathLength; i++ ) {
            // rest are relative to start
            char *stepString = autoSprintf( " %d %d", 
                                         ourLiveObject->pathToDest[i].x
                                            - ourLiveObject->pathToDest[0].x,
                                         ourLiveObject->pathToDest[i].y
                                            - ourLiveObject->pathToDest[0].y );
            
            moveMessageBuffer.appendElementString( stepString );
            delete [] stepString;
            }
        moveMessageBuffer.appendElementString( "#" );
        

        char *message = moveMessageBuffer.getElementString();
        sendToSocket( mServerSocket, (unsigned char*)message, 
                      strlen( message ) );
            
        numServerBytesSent += strlen( message );
        overheadServerBytesSent += 52;

        delete [] message;

        // start moving before we hear back from server



        
        

        ourLiveObject->moveTotalTime = 
            ourLiveObject->pathLength / 
            ourLiveObject->lastSpeed;

        ourLiveObject->moveEtaTime = game_getCurrentTime() +
            ourLiveObject->moveTotalTime;

            
        updateMoveSpeed( ourLiveObject );
        }    
    }




void LivingLifePage::pointerDrag( float inX, float inY ) {
    }

void LivingLifePage::pointerUp( float inX, float inY ) {
    }

void LivingLifePage::keyDown( unsigned char inASCII ) {

    switch( inASCII ) {
        case 'e':
        case 'E':
            mEKeyDown = true;
            break;
        case 13:  // enter
            // speak
            if( ! mSayField.isFocused() ) {
                
                mSayField.setText( "" );
                mSayField.focus();
                }
            else {
                char *typedText = mSayField.getText();
                
                if( strcmp( typedText, "" ) == 0 ) {
                    mSayField.unfocus();
                    }
                else {
                    // send text to server
                    char *message = 
                        autoSprintf( "SAY 0 0 %s\n#",
                                     typedText );
                    sendToSocket( mServerSocket, (unsigned char*)message, 
                                  strlen( message ) );
            
                    numServerBytesSent += strlen( message );
                    overheadServerBytesSent += 52;

                    
                    mSayField.setText( "" );
                    mSayField.unfocus();

                    delete [] message;
                    }
                
                delete [] typedText;
                }
            break;
        }
    }

        
void LivingLifePage::keyUp( unsigned char inASCII ) {

    switch( inASCII ) {
        case 'e':
        case 'E':
            mEKeyDown = false;
            break;
        }

    }

        
