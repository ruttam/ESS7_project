//This file was generated from (Commercial) UPPAAL 4.0.14 (rev. 5615), May 2014

/*

*/
E<> Process(1).RUNNING

/*

*/
Pr[ <= 1000] (<> proc0.RUNNING and proc1.RUNNING)

/*

*/
A[] not (proc0.RUNNING and proc1.RUNNING)

/*

*/
A[] not deadlock

/*

*/
E<> partition1.COLD

/*

*/
E<> partition1.NORMAL

/*

*/
E<> partition1.IDLE 
