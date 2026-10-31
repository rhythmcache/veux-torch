package com.example.veuxtorch;

interface IFlashService {
    int turnOn(int level);
    int setLevel(int level);
    int turnOff();
    int heartbeat();
    int getMaxLevel();
    int[] getStatus();
    String getErrorMessage(int err);
}
