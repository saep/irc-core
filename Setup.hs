{-|
Module      : Main
Description : Custom setup script
Copyright   : (c) Eric Mertens, 2016
License     : ISC
Maintainer  : emertens@gmail.com

This is a default setup script except that it checks that all
transitive dependencies of this package use free licenses.

-}

module Main (main) where

import DependencyTools

main = defaultDependencyToolsMain
