package mmo.activityext.newact20240705.nationalcompetition.model
{
   import ash.signals.Signal0;
   import mmo.activityext.newact20240705.nationalcompetition.config.NC_BonusConfig;
   import mmo.activityext.newact20240705.nationalcompetition.config.NC_Config;
   import mmo.activityext.newact20240705.nationalcompetition.model.reward.NC_Reward;
   import mmo.activityext.newact20240705.nationalcompetition.model.reward.NC_YuanHunReward;
   import mmo.common.aoqi.reward.ActRewardsData;
   import mmo.common.helper.ActUtil;
   import mmo.common.utils.dt.mvc.Model;
   import mmo.interfaces.ServiceContainer;
   import mmo.interfaces.configureformation.IConfigureFormation;
   import mmo.interfaces.formation.Formation;
   import mmo.interfaces.formationpanel.IFormationPanelService;
   import mmo.interfaces.ideal.pet.IIdealPetService;
   import mmo.interfaces.interacthelper.IInteractHelper;
   import mmo.interfaces.multipleformpet.IMultipleFormPetService;
   import mmo.play.dialog.InteractUtil;
   import mmo.play.globalfunctions.forSvc;
   
   public class NC_Model extends Model implements NC_IModel
   {
      
      private var _updateSignal:Signal0;
      
      private var _interactHelper:IInteractHelper = ServiceContainer.getService(IInteractHelper);
      
      private var _changeSetId:int;
      
      private var _client:NC_Client;
      
      private var _reward:ActRewardsData;
      
      private var _essenceReward:ActRewardsData;
      
      private var _myScore:int;
      
      private var _myYuanHunTimes:int;
      
      private var _ticketNum:int;
      
      private var _weekTicketNum:int;
      
      private var _dailyTicketNum:int;
      
      private var _choosePetData:NC_ChoosePetData;
      
      private var _battleData:NC_BattleData;
      
      private var _state:int;
      
      private var _totalTopTimes:int;
      
      private var _buyTicketTimes:int;
      
      private var _buyDailyTicketTimes:int;
      
      private var _summaryWinTimes:int;
      
      public function NC_Model()
      {
         super(NC_ModelEvent);
         this._client = new NC_Client();
         this._updateSignal = new Signal0();
         this._choosePetData = new NC_ChoosePetData();
         this._choosePetData.changeState.add(this.changeStateToBattle);
         this._battleData = new NC_BattleData();
         this._battleData._changeState.add(this.changeStateToGetBonus);
         this._battleData._afterFight.add(this.afterFight);
      }
      
      public function init(param1:Object) : void
      {
         this._reward = new ActRewardsData().init(NC_BonusConfig.bonusConfig,param1,NC_Reward);
         this._essenceReward = new ActRewardsData().init(NC_BonusConfig.essenceConfig,param1["pool"][0],NC_YuanHunReward);
         this._totalTopTimes = param1.cc;
         this._summaryWinTimes = param1.twc;
         this._myScore = param1.ltc;
         this._myYuanHunTimes = param1["pool"][0]["gt"];
         this._ticketNum = param1.wwt;
         this._weekTicketNum = param1.rwwt;
         this._dailyTicketNum = param1.rdt;
         this._buyTicketTimes = param1.wwb;
         this._buyDailyTicketTimes = param1.rdb;
         var _loc2_:int = int(param1.s);
         var _loc3_:Array = param1.rpes;
         var _loc4_:Array = param1.spes;
         var _loc5_:int = Boolean(param1.sid) ? int(param1.sid) : -1;
         this._choosePetData.init(_loc3_,_loc4_,_loc5_);
         this.initBattleData(param1);
         if(Boolean(param1.hf))
         {
            this._state = NC_Config.STATE_GET_BONUS;
         }
         else if(_loc2_ > 0)
         {
            this._state = NC_Config.STATE_CHOOSE_BUFF + _loc2_ - 1;
            if(this._state == NC_Config.STATE_RESET_FORMATION)
            {
               this._battleData.updateFormationId(param1.fid);
            }
         }
         else if(_loc4_.length <= 0 && this._battleData._formationId <= 0)
         {
            this._state = NC_Config.STATE_NOT_START;
         }
         else if(_loc4_.length <= 0)
         {
            this._state = NC_Config.STATE_RONDOM_FORMATION;
         }
         else if(_loc4_.length < NC_Config.CHOOSE_PET_NUM)
         {
            this._state = NC_Config.STATE_CHOOSE_PET;
         }
         else
         {
            this._state = NC_Config.STATE_BATTLE;
         }
      }
      
      private function initBattleData(param1:Object) : void
      {
         this._battleData.parse(null,param1,this._client);
      }
      
      public function fight(param1:Object) : void
      {
         this._battleData.handleBeforeFight(param1);
         this._choosePetData.options = param1.rpes;
      }
      
      private function changeStateToGetBonus(param1:Object = null) : void
      {
         this._state = NC_Config.STATE_GET_BONUS;
         InteractUtil.applyCallback(param1["callback"]);
      }
      
      private function afterFight(param1:Boolean, param2:Function) : void
      {
         if(!param1)
         {
            this._state = NC_Config.STATE_CHOOSE_REPLNC_E_PET;
         }
         InteractUtil.applyCallback(param2);
      }
      
      private function changeStateToBattle(param1:Function) : void
      {
         this._state = NC_Config.STATE_CHOOSE_BUFF;
         InteractUtil.applyCallback(param1);
      }
      
      public function msgSetFormation(param1:Function) : void
      {
         var onSetFormation:Function = null;
         var callback:Function = param1;
         onSetFormation = function(param1:Object):void
         {
            _state = NC_Config.STATE_BATTLE;
            InteractUtil.applyCallback(callback);
         };
         var formation:Formation = this._battleData._formationModel.getFormationById(this._battleData._formationModel.currentFormationId);
         this._client.msgSetFormation(onSetFormation,this._battleData.getPosStr(),formation.transformSkillToString(),formation.psychicsSkill,formation.heroSkill);
      }
      
      public function msgChoosePet(param1:Function, param2:int, param3:int) : void
      {
         var iRes:int = 0;
         var onChoosePet:Function = null;
         var callback:Function = param1;
         var index:int = param2;
         var pos:int = param3;
         onChoosePet = function(param1:Object):void
         {
            var params:Object = param1;
            if(params.rpos != null)
            {
               _battleData.updatePreSelectBuff((params.rpos as Array).join(","));
            }
            _choosePetData.handleChoose(params,index,function():void
            {
               if(iRes == 1)
               {
                  _battleData.updateHeroSkill(callback);
               }
               else if(iRes == 2)
               {
                  _battleData.updatePsychicsSkill(callback);
               }
               else if(iRes == 3)
               {
                  _battleData.updateTransformSkill(callback);
               }
               else
               {
                  InteractUtil.applyCallback(callback);
               }
            });
         };
         var petId:int = int(this._choosePetData.options[index]);
         iRes = this._battleData.addPet(petId,pos);
         if(iRes == -1)
         {
            return;
         }
         this._client.msgChoosePet(onChoosePet,index,this._battleData.getPosStr());
      }
      
      public function msgReChoosePet(param1:Function, param2:int, param3:int) : void
      {
         var iRes:int = 0;
         var onChoosePet:Function = null;
         var callback:Function = param1;
         var index:int = param2;
         var pos:int = param3;
         onChoosePet = function(param1:Object):void
         {
            _choosePetData.replacePet(index);
            _state = NC_Config.STATE_CHOOSE_BUFF;
            if(param1.rpos != null)
            {
               _battleData.updatePreSelectBuff((param1.rpos as Array).join(","));
            }
            if(iRes == 1)
            {
               _battleData.updateHeroSkill(callback);
            }
            else if(iRes == 2)
            {
               _battleData.updatePsychicsSkill(callback);
            }
            else if(iRes == 3)
            {
               _battleData.updateTransformSkill(callback);
            }
            else
            {
               InteractUtil.applyCallback(callback);
            }
         };
         var petId:int = int(this._choosePetData.hadChoose[this._choosePetData.replaceIndex - 1]);
         var newId:int = int(this._choosePetData.options[index]);
         iRes = this._battleData.replacePet(index,pos,petId,newId,this._choosePetData.replaceIndex);
         this._client.choosePet2Change(onChoosePet,index,this._choosePetData.replaceIndex,this._battleData.getPosStr());
      }
      
      public function msgGiveUpReChoosePet(param1:Function) : void
      {
         var onChoosePet:Function = null;
         var callback:Function = param1;
         onChoosePet = function(param1:Object):void
         {
            _state = NC_Config.STATE_CHOOSE_BUFF;
            if(param1.rpos != null)
            {
               _battleData.updatePreSelectBuff((param1.rpos as Array).join(","));
            }
            InteractUtil.applyCallback(callback);
         };
         this._client.giveUpChoosePet2Change(onChoosePet);
      }
      
      public function msgChooseBuff(param1:int, param2:Function) : void
      {
         var buffId:int = 0;
         var onSelectPower:Function = null;
         var index:int = param1;
         var callback:Function = param2;
         onSelectPower = function(param1:Object):void
         {
            if(buffId == 1)
            {
               _state = NC_Config.STATE_RESET_FORMATION;
            }
            else if(buffId == 2)
            {
               _state = NC_Config.STATE_RECHOOSE_FORMATION;
            }
            else
            {
               _state = NC_Config.STATE_BATTLE;
            }
            _battleData.addBuff(index);
            InteractUtil.applyCallback(callback);
         };
         buffId = int(this._battleData._preSelectBuff[index]);
         this._client.msgSelectPower(onSelectPower,buffId);
      }
      
      public function msgGetInfo(param1:Function) : void
      {
         var callback:Function = param1;
         forSvc([IFormationPanelService,IConfigureFormation,IMultipleFormPetService,IIdealPetService],function():void
         {
            getInfo(callback);
         });
      }
      
      private function getInfo(param1:Function) : void
      {
         var onGetInfo:Function = null;
         var callback:Function = param1;
         onGetInfo = function(param1:Object):void
         {
            init(param1);
            InteractUtil.applyCallback(callback);
         };
         this._client.getInfo(onGetInfo);
      }
      
      public function msgRandom3Pets(param1:Function) : void
      {
         var onRandom3Pets:Function = null;
         var callback:Function = param1;
         onRandom3Pets = function(param1:Object):void
         {
            _state = NC_Config.STATE_CHOOSE_PET;
            _choosePetData.options = param1.rpes;
            ActUtil.applyCallback(callback);
         };
         this._client.msgRandom3Pets(onRandom3Pets);
      }
      
      public function msgResetFormation(param1:Function) : void
      {
         var onResetFormation:Function = null;
         var callback:Function = param1;
         onResetFormation = function(param1:Object):void
         {
            _state = NC_Config.STATE_RESET_FORMATION;
            _battleData.updateFormationId(param1.fid);
            InteractUtil.applyCallback(callback);
         };
         this._client.msgChangeFormation(onResetFormation);
      }
      
      public function msgRandomFormation(param1:Function) : void
      {
         var onRandomFormation:Function = null;
         var callback:Function = param1;
         onRandomFormation = function(param1:Object):void
         {
            ++_ticketNum;
            --_weekTicketNum;
            ++_dailyTicketNum;
            _state = NC_Config.STATE_RONDOM_FORMATION;
            _battleData.updateFormationId(param1.fid);
            ActUtil.applyCallback(callback);
         };
         this._client.getRandomFormation(onRandomFormation);
      }
      
      public function msgBuyTicket(param1:Function) : void
      {
         var onBuyTicket:Function = null;
         var callback:Function = param1;
         onBuyTicket = function(param1:Object):void
         {
            ++_buyTicketTimes;
            ++_weekTicketNum;
            ++_buyDailyTicketTimes;
            InteractUtil.applyCallback(callback);
         };
         this._client.msgBuyTicket(onBuyTicket);
      }
      
      public function msgResetGame(param1:Function) : void
      {
         var onResetGame:Function = null;
         var callback:Function = param1;
         onResetGame = function(param1:Object = null):void
         {
            _state = NC_Config.STATE_NOT_START;
            _totalTopTimes = param1["cc"];
            _summaryWinTimes = param1["twc"];
            _myScore = param1["ltc"];
            _choosePetData.init([],[],-1);
            _battleData.resetData();
            InteractUtil.applyCallback(callback);
         };
         this._changeSetId = this._interactHelper.getNewChangeSetId();
         this._client.msgResetGame(onResetGame);
      }
      
      public function getBonus(param1:NC_Reward, param2:Object, param3:Function) : void
      {
         var reward:NC_Reward = param1;
         var p2Server:Object = param2;
         var doNext:Function = param3;
         this._client.msgGetBonus(p2Server,function():void
         {
            if(reward.isTimesReward)
            {
               _myYuanHunTimes += reward.childRewardTimes / 5;
            }
            doNext();
         });
      }
      
      public function getChildBonus(param1:Object, param2:Function = null) : void
      {
         var p2Server:Object = param1;
         var doNext:Function = param2;
         this._client.msgGetChildBonus(p2Server,function():void
         {
            --_myYuanHunTimes;
            _updateSignal.dispatch();
            ActUtil.applyCallback(doNext);
         });
      }
      
      public function getRankData(param1:int, param2:Function = null) : void
      {
         this._client.msgGetRank(param2,param1);
      }
      
      public function dispose() : void
      {
         this._battleData.dispose();
      }
      
      public function get totalTopTimes() : int
      {
         return this._totalTopTimes;
      }
      
      public function get buyTicketTimes() : int
      {
         return this._buyTicketTimes;
      }
      
      public function get buyDailyTicketTimes() : int
      {
         return this._buyDailyTicketTimes;
      }
      
      public function get summaryWinTimes() : int
      {
         return this._summaryWinTimes;
      }
      
      public function get changeSetId() : int
      {
         return this._changeSetId;
      }
      
      public function get ticketNum() : int
      {
         return this._ticketNum;
      }
      
      public function get weekTicketNum() : int
      {
         return Math.max(0,this._weekTicketNum);
      }
      
      public function get dailyTicketNum() : int
      {
         return this._dailyTicketNum;
      }
      
      public function get myScore() : int
      {
         return this._myScore;
      }
      
      public function get myYuanHunTimes() : int
      {
         return this._myYuanHunTimes;
      }
      
      public function get state() : int
      {
         return this._state;
      }
      
      public function get choosePetData() : NC_ChoosePetData
      {
         return this._choosePetData;
      }
      
      public function get battleData() : NC_BattleData
      {
         return this._battleData;
      }
      
      public function get updateSignal() : Signal0
      {
         return this._updateSignal;
      }
      
      public function get reward() : ActRewardsData
      {
         return this._reward;
      }
      
      public function get uniqueReward() : Array
      {
         var hasTimesReward:Boolean = false;
         hasTimesReward = false;
         return this._reward.getRewards(function(param1:NC_Reward, ... rest):Boolean
         {
            var _loc3_:* = undefined;
            if(param1.isTimesReward)
            {
               _loc3_ = !hasTimesReward;
               hasTimesReward = true;
            }
            else
            {
               _loc3_ = param1.isOnly;
            }
            return _loc3_;
         });
      }
      
      public function get essenceTimesReward() : Array
      {
         return this._reward.getRewards(function(param1:NC_Reward, ... rest):Boolean
         {
            return param1.isTimesReward;
         });
      }
      
      public function get essenceReward() : ActRewardsData
      {
         return this._essenceReward;
      }
   }
}

