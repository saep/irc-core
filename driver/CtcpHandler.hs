{-# LANGUAGE OverloadedStrings #-}
module CtcpHandler (ctcpHandler) where

import Control.Lens
import Control.Monad
import Data.ByteString (ByteString)
import Data.Monoid
import Data.Time
import Data.Version (showVersion)
import System.Locale
import qualified Data.ByteString.Char8 as B8

import ClientState
import Irc.Format
import Irc.Message
import Irc.Cmd
import Paths_irc_core (version)

versionString :: ByteString
versionString = "glirc " <> B8.pack (showVersion version)

sourceString :: ByteString
sourceString = "https://github.com/glguy/irc-core"

ctcpHandler :: EventHandler
ctcpHandler = EventHandler
  { _evName = "CTCP replies"
  , _evOnEvent = \_ msg st ->

       do let sender = views mesgSender userNick msg
          forOf_ (mesgType . _CtcpReqMsgType) msg $ \(command,params) ->

               -- Don't send responses to ignored users
               unless (view (clientIgnores . contains sender) st) $
                 case command of
                   "VERSION" ->
                     clientSend (ctcpResponseCmd sender "VERSION" versionString) st
                   "PING" ->
                     clientSend (ctcpResponseCmd sender "PING" params) st
                   "SOURCE" ->
                     clientSend (ctcpResponseCmd sender "SOURCE" sourceString) st
                   "TIME" -> do
                     now <- getZonedTime
                     let resp = formatTime defaultTimeLocale "%a %d %b %Y %T %Z" now
                     clientSend (ctcpResponseCmd sender "TIME" (B8.pack resp)) st
                   _ -> return ()

          -- reschedule handler
          return (over clientAutomation (cons ctcpHandler) st)
  }
