{-# Options_GHC -Wno-unused-do-bind #-}

module Client.NetworkConnection
  ( NetworkConnection(..)
  , NetworkId
  , NetworkEvent(..)
  , createConnection
  , send
  ) where

import           Control.Concurrent
import           Control.Concurrent.STM
import           Control.Concurrent.Async
import           Control.Exception
import           Control.Monad
import           Data.ByteString (ByteString)
import qualified Data.ByteString as B
import           Data.Time
import           Network.Connection

import           Irc.RateLimit
import           Client.Connect
import           Client.ServerSettings

type NetworkId = Int

data NetworkConnection = NetworkConnection
  { connOutQueue :: !(TChan ByteString)
  , abortConnection :: !(IO ())
  , connId       :: !NetworkId
  }

data NetworkEvent
  = NetworkLine  !NetworkId !ZonedTime !ByteString
  | NetworkError !NetworkId !ZonedTime !SomeException
  | NetworkClose !NetworkId !ZonedTime

instance Show NetworkConnection where
  showsPrec p _ = showParen (p > 10)
                $ showString "NetworkConnection _"

send :: NetworkConnection -> ByteString -> IO ()
send c msg = atomically (writeTChan (connOutQueue c) msg)

createConnection ::
  NetworkId ->
  ConnectionContext ->
  ServerSettings ->
  TChan NetworkEvent ->
  IO NetworkConnection
createConnection network cxt settings inQueue =
   do outQueue <- atomically newTChan

      supervisor <- async (startConnection network cxt settings inQueue outQueue)

      -- Having this reporting thread separate from the supervisor ensures
      -- that canceling the supervisor with abortConnection doesn't interfere
      -- with carefully reporting the outcome
      forkIO $ do outcome <- waitCatch supervisor
                  case outcome of
                    Right{} -> recordNormalExit
                    Left e  -> recordFailure e

      return NetworkConnection
        { connOutQueue    = outQueue
        , abortConnection = cancel supervisor
        , connId          = network
        }
  where
    recordFailure :: SomeException -> IO ()
    recordFailure ex =
        do now <- getZonedTime
           atomically (writeTChan inQueue (NetworkError network now ex))

    recordNormalExit :: IO ()
    recordNormalExit =
      do now <- getZonedTime
         atomically (writeTChan inQueue (NetworkClose network now))


startConnection ::
  NetworkId ->
  ConnectionContext ->
  ServerSettings ->
  TChan NetworkEvent ->
  TChan ByteString ->
  IO ()
startConnection network cxt settings onInput outQueue =
  do rate <- newRateLimitDefault
     withConnection cxt settings $ \h ->
       withAsync (sendLoop h outQueue rate)      $ \sender ->
       withAsync (receiveLoop network h onInput) $ \receiver ->
         do res <- waitEitherCatch sender receiver
            case res of
              Left  Right{}  -> fail "PANIC: sendLoop returned"
              Right Right{}  -> return ()
              Left  (Left e) -> throwIO e
              Right (Left e) -> throwIO e

sendLoop :: Connection -> TChan ByteString -> RateLimit -> IO ()
sendLoop h outQueue rate =
  forever $
    do msg <- atomically (readTChan outQueue)
       tickRateLimit rate
       connectionPut h msg

ircMaxMessageLength :: Int
ircMaxMessageLength = 512

receiveLoop :: NetworkId -> Connection -> TChan NetworkEvent -> IO ()
receiveLoop network h inQueue =
  do msg <- connectionGetLine ircMaxMessageLength h
     unless (B.null msg) $
       do now <- getZonedTime
          atomically (writeTChan inQueue (NetworkLine network now (B.init msg)))
          receiveLoop network h inQueue
