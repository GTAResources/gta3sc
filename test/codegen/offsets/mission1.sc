MISSION_START
PRINT_HELP miss1

miss1_label1:
IS_BUTTON_PRESSED PAD1 RIGHTSHOCK
GOTO_IF_FALSE miss1_label2
GOTO miss1_label3

miss1_label2:
WAIT 0
GOTO miss1_label1

miss1_label3:
MISSION_END