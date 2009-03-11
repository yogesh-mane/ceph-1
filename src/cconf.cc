// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "config.h"
#include "common/ConfUtils.h"

void usage() 
{
  cerr << "usage: cconf [-c filename] [-l|--list_sections <prefix>] [-s <section>] [[-s section] ... ] <key> [default]" << std::endl;
  exit(1);
}

int main(int argc, const char **argv) 
{
  const char *fname = g_conf.conf;
  const char *key = NULL, *defval = NULL;
  const char *list_sections = 0;
  char *val;
  int param = 0;
  vector<const char*> args;
  vector<const char *> sections;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);

  if (args.size() < 2)
    usage();

  for (unsigned i=0; i<args.size(); i++) {
    if (strcmp(args[i], "-c") == 0) {
      if (i < args.size() - 1)
        fname = args[++i];
      else
	usage();
    } else if (strcmp(args[i], "-l") == 0 ||
	       strcmp(args[i], "--list_sections") == 0) {
      if (i < args.size() - 1)
	list_sections = args[++i];
      else
	usage();
    } else if (strcmp(args[i], "-s") == 0) {
      if (param == 0)
          param++;
      if (i < args.size() - 1)
        sections.push_back(args[++i]);
      else
	usage();
    } else {
      switch (param) {
      	case 0:
	    sections.push_back(args[i]);
	    break;
	case 1:
	    key = args[i];
	    break;
	case 2:
	    defval = args[i];
	    break;
      }
      param++;
    }
  }

  if (!list_sections && (param < 1 || param > 3))
    usage();

  ConfFile cf(fname);
  parse_config_file(&cf, true);

  if (list_sections) {
    for (std::list<ConfSection*>::const_iterator p = cf.get_section_list().begin();
	 p != cf.get_section_list().end();
	 p++) {
      if (strncmp(list_sections, (*p)->get_name().c_str(), strlen(list_sections)) == 0)
	cout << (*p)->get_name() << std::endl;
    }
    return 0;
  }

  for (unsigned i=0; i<sections.size(); i++) {
    cf.read(sections[i], key, (char **)&val, NULL);

    if (val) {
      cout << val << std::endl;
      exit(0);
    }
  }

  if (defval) {
    cout << defval << std::endl;
    exit(0);
  }

  exit(1);
}
