#!/bin/bash
#
# Copyright 2012 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Generate version information for the EC binary

# Use this symbol as a separator to be able to reliably concatenate strings of
# text.
dc=$'\001'

# Default marker to indicate 'dirty' repositories
dirty_marker='+'

# This function examines the state of the current directory and attempts to
# extract its version information: the latest tag, if any, how many patches
# are there since the latest tag, the top sha1, and if there are local
# modifications.
#
# Local modifications are reported by concatenating the revision string and
# the string '-dirty' using the $dc symbol as the separator.
#
# If there is no tags defined in this git repository, the base version is
# considered to be 0.0.
#
# If current directory is not a git depository, this function prints out
# "no_version"

get_tree_version() {
  local marker
  local ghash
  local numcommits
  local tag
  local vbase
  local ver_branch
  local ver_major

  if ghash=`git rev-parse --short --verify HEAD 2>/dev/null`; then
    if gdesc=`git describe --dirty --match='v*' 2>/dev/null`; then
      IFS="-" fields=($gdesc)
      tag="${fields[0]}"
      IFS="." vernum=($tag)
      numcommits=$((${vernum[2]}+${fields[1]:-0}))
      ver_major="${vernum[0]}"
      ver_branch="${vernum[1]}"
    else
      numcommits=`git rev-list HEAD | wc -l`
      ver_major="v0"
      ver_branch="0"
    fi
    # avoid putting the -dirty attribute if only the timestamp
    # changed
    git status > /dev/null 2>&1

    if [ -n "$(git diff-index --name-only HEAD 2>/dev/null)" ]; then
      marker="${dirty_marker}"
    else
      marker="-"
    fi
    vbase="${ver_major}.${ver_branch}.${numcommits}${marker}${ghash}"
  else
    # Fall back to the VCSID provided by the packaging system if available.
    if ghash=${VCSID##*-}; then
      vbase="1.1.9999-${ghash:0:7}"
    else
      # then ultimately fails to "no_version"
      vbase="no_version"
    fi
  fi
  if [[ "${marker}" == "${dirty_marker}" ]]; then
      echo "${vbase}${dc}${marker}"
  else
      echo "${vbase}${dc}"
  fi
}


main() {
  local component
  local dir_list
  local gitdate
  local most_recents
  local most_recent_file
  local root
  local timestamp
  local tool_ver
  local values
  local vbase
  local ver

  IFS="${dc}"
  ver="${CR50_DEV:+DBG/}${CRYPTO_TEST:+CT/}${BOARD}_"
  tool_ver=""
  most_recents=()    # Non empty if any of the component repos is 'dirty'.
  dir_list=( . )   # list of component directories, always includes the EC tree

  if [[ -n ${BOARD} ]]; then
    case "${BOARD}" in
      (cr50)
        dir_list+=( ../../third_party/tpm2 ../../third_party/cryptoc )
        ;;
      (*_fp)
        dir_list+=( ./private )
        ;;
      (*)
        # For private-crX boards add their git root and cryptoc.
        for root in private-cr5*; do
          if [[ -d "${root}/board/${BOARD}" ]]; then
            dir_list+=( "${root}" ../../third_party/cryptoc )
          fi
        done
        ;;
    esac
  fi
  # Create a combined version string for all component directories.
  for git_dir in ${dir_list[@]}; do
    pushd "${git_dir}" > /dev/null
    component="$(basename "${git_dir}")"
    values=( $(get_tree_version) )
    vbase="${values[0]}"             # Retrieved version information.
    if [[ -n "${values[1]}" ]]; then
      # From each modified repo get the most recently modified file.
      most_recent_file="$(git status --porcelain | \
                                       awk '$1 ~ /[M|A|?]/ {print $2}' |  \
                                       xargs ls -t | head -1)"
      most_recents+=("$(realpath "${most_recent_file}")")
    fi
    if [ "${component}" != "." ]; then
      ver+=" ${component}:"
    fi
    ver+="${vbase}"
    tool_ver+="${vbase}"
    popd > /dev/null
  done

  # On some boards where the version number consists of multiple components we
  # want to separate the first word of the version string as the version of the
  # EC tree.
  IFS=' ' first_word=(${ver})

  echo "/* This file is generated by util/getversion.sh */"

  echo "/* Version string, truncated to 31 chars (+ terminating null = 32) */"
  echo "#define CROS_EC_VERSION32 \"${first_word:0:31}\""

  echo "/* Version string for ectool. */"
  echo "#define CROS_ECTOOL_VERSION \"${tool_ver}\""

  echo "/* Version string for stm32mon. */"
  echo "#define CROS_STM32MON_VERSION \"${tool_ver}\""

  echo "/* Sub-fields for use in Makefile.rules and to form build info string"
  echo " * in common/version.c. */"
  echo "#define VERSION \"${ver}\""
  if [ "$REPRODUCIBLE_BUILD" = 1 ]; then
    echo '#define BUILDER "reproducible@build"'
  else
    echo "#define BUILDER \"${USER}@`hostname`\""
  fi

  if [[ ${#most_recents[@]} != 0 ]]; then
    # There are modified files, use the timestamp of the most recent one as
    # the build version timestamp.
    most_recent_file="$(ls -t "${most_recents[@]}" | head -1)"
    timestamp="$(stat -c '%y' "${most_recent_file}" | sed 's/\..*//')"
    echo "/* Repo is dirty, using time of most recent file modification. */"
    echo "#define DATE \"${timestamp}\""
  else
    echo "/* Repo is clean, use the commit date of the last commit. */"
    # If called from an ebuild we won't have a git repo, so redirect stderr
    # to avoid annoying 'Not a git repository' errors.
    gitdate="$(
      for git_dir in "${dir_list[@]}"; do
        git -C "${git_dir}" log -1 --format='%ct %ci' HEAD 2>/dev/null
      done | sort | tail -1 | cut -d ' ' -f '2 3')"
    echo "#define DATE \"${gitdate}\""
  fi
}

main
