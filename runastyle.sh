#!/bin/bash
git ls-files '*.[ch]' | xargs astyle --options=.astylerc >> astyle.log
