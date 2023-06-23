#!/usr/bin/env python3

import sys

with open("kernel.config") as file:

	with open(sys.argv[1], "w") as header:
		# TODO: Print copyright header?
		header.write("#ifndef _ONYX_CONFIG_H\n#define _ONYX_CONFIG_H\n\n")
		for line_nr, line in enumerate(file, start=1):
			line = line.replace('=', ' ')
			tokens = line.split()
			if len(tokens) == 1:
				print(f"Error: Bad config line at line {str(line_nr)}" + "\n")
				exit(1)

			if tokens[1] != "n":
				header.write(f'#define {line}')

		header.write("\n#endif\n")