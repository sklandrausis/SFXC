/* Copyright (c) 2007 Joint Institute for VLBI in Europe (Netherlands)
#ifndef CHANNEL_EXTRACTOR_FAST_H__
/*******************************************************************************
*
* @class Channel_extractor_fast
* @desc The fastest static channel extractor.
* The performance dependon 
* the bit pattern. With the worst bit pattern the performance on 
* an IntelCore2 is from [60MB/s (16 channel) to 130MB/s]. 
* On DAS3 cut the number by 2 to get a rule of thumb approximation. Don't
* be worried by the compile time. Everything that is done at compiled time...
* will not to be done at run time :)
*******************************************************************************/